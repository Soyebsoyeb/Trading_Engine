// trading_engine.cpp
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <sstream>
#include <iomanip>

using namespace std;
using steady_clock = chrono::high_resolution_clock;
using ns = chrono::nanoseconds;

// ---------- Order ----------
struct Order {
    int id;
    char side; // 'B' or 'S'
    double price;
    int quantity;
    steady_clock::time_point created;
};

// ---------- Thread-safe blocking queue with notification ----------
class OrderQueue {
    queue<Order> q;
    mutex mtx;
    condition_variable cv;
    atomic<size_t> size_{0};

public:
    void push(Order &&order) {
        {
            lock_guard<mutex> lock(mtx);
            q.push(std::move(order));
            size_.fetch_add(1, memory_order_relaxed);
        }
        cv.notify_one();
    }

    // pop up to n orders into dest; blocks until at least one order or done==true
    size_t pop_batch(vector<Order> &dest, size_t max_items, atomic<bool> &done) {
        unique_lock<mutex> lock(mtx);
        cv.wait_for(lock, chrono::milliseconds(50), [&]{ return !q.empty() || done.load(); });
        size_t count = 0;
        while(count < max_items && !q.empty()) {
            dest.push_back(std::move(q.front()));
            q.pop();
            ++count;
            size_.fetch_sub(1, memory_order_relaxed);
        }
        return count;
    }

    size_t size() const { return size_.load(memory_order_relaxed); }

    bool empty() {
        lock_guard<mutex> lock(mtx);
        return q.empty();
    }

    void notify_all() { cv.notify_all(); }
};

// ---------- OrderBook (simple but thread-safe) ----------
class OrderBook {
    vector<Order> buys;  // buy orders
    vector<Order> sells; // sell orders
    mutable mutex ob_mtx;

public:
    // Statistics
    atomic<long long> totalTrades{0};
    atomic<long long> buyVolume{0};
    atomic<long long> sellVolume{0};
    atomic<long long> processedOrders{0};
    atomic<long long> totalQueueLatencyNs{0};

    // limited trade log
    mutex log_mtx;
    deque<string> tradeLog;
    size_t tradeLogLimit = 2000;

    void addOrder(const Order &o) {
        lock_guard<mutex> lock(ob_mtx);
        if(o.side == 'B') buys.push_back(o);
        else sells.push_back(o);
        processedOrders.fetch_add(1, memory_order_relaxed);
    }

    // match orders; caller should be serialized by user (we lock internally)
    void matchOrders() {
        lock_guard<mutex> lock(ob_mtx);

        auto buy_cmp = [](const Order &a, const Order &b){
            if (a.price != b.price) return a.price > b.price;
            return a.created < b.created;
        };
        auto sell_cmp = [](const Order &a, const Order &b){
            if (a.price != b.price) return a.price < b.price;
            return a.created < b.created;
        };

        sort(buys.begin(), buys.end(), buy_cmp);
        sort(sells.begin(), sells.end(), sell_cmp);

        while(!buys.empty() && !sells.empty() && buys.front().price >= sells.front().price) {
            Order &b = buys.front();
            Order &s = sells.front();
            int tradeQty = min(b.quantity, s.quantity);
            double tradePrice = s.price; // price setter strategy: use sell price

            // Stats
            totalTrades.fetch_add(1, memory_order_relaxed);
            buyVolume.fetch_add(tradeQty, memory_order_relaxed);
            sellVolume.fetch_add(tradeQty, memory_order_relaxed);

            // Log trade (thread-safe)
            {
                ostringstream oss;
                auto t = chrono::duration_cast<chrono::milliseconds>(steady_clock::now().time_since_epoch()).count();
                oss << "T@" << t << " BUY_ID=" << b.id << " SELL_ID=" << s.id
                    << " Q=" << tradeQty << " P=" << fixed << setprecision(2) << tradePrice;
                lock_guard<mutex> lg(log_mtx);
                tradeLog.push_back(oss.str());
                if(tradeLog.size() > tradeLogLimit) tradeLog.pop_front();
            }

            b.quantity -= tradeQty;
            s.quantity -= tradeQty;

            if(b.quantity == 0) buys.erase(buys.begin());
            if(s.quantity == 0) sells.erase(sells.begin());
        }
    }

    vector<string> getRecentTrades(size_t n=20) {
        lock_guard<mutex> lg(log_mtx);
        vector<string> out;
        size_t start = (tradeLog.size() > n) ? tradeLog.size()-n : 0;
        for(size_t i = start; i < tradeLog.size(); ++i) out.push_back(tradeLog[i]);
        return out;
    }

    size_t buyDepth() const { lock_guard<mutex> l(ob_mtx); return buys.size(); }
    size_t sellDepth() const { lock_guard<mutex> l(ob_mtx); return sells.size(); }
};

// ---------- Market producer ----------
void marketProducer(OrderQueue &oq, int numOrders, int idStart,
                    int delayMicros, atomic<bool> &stopFlag)
{
    for(int i = 0; i < numOrders && !stopFlag.load(); ++i) {
        Order o;
        o.id = idStart + i;
        o.side = (i % 2 == 0) ? 'B' : 'S';
        o.price = 100.0 + (i % 20) + ((i/50) * 0.01); // add micro price variance
        o.quantity = 1 + (i % 10);
        o.created = steady_clock::now();

        oq.push(std::move(o));
        if(delayMicros > 0) this_thread::sleep_for(chrono::microseconds(delayMicros));
    }
}

// ---------- Matching engine worker ----------
void matchingEngineWorker(OrderQueue &oq, OrderBook &ob, atomic<bool> &done,
                          int batchSize, atomic<bool> &stopFlag)
{
    vector<Order> batch;
    batch.reserve(batchSize);

    while(!done.load() || !oq.empty()) {
        batch.clear();
        size_t got = oq.pop_batch(batch, batchSize, done);
        if(got == 0) {
            if(done.load()) break;
            continue;
        }

        // record queue latency sum
        auto now = steady_clock::now();
        long long localLatencyNs = 0;
        for(auto &o : batch) {
            auto qlat = chrono::duration_cast<ns>(now - o.created).count();
            localLatencyNs += qlat;
            ob.totalQueueLatencyNs.fetch_add(qlat, memory_order_relaxed);
        }

        // add to book and match
        for(const auto &o: batch) ob.addOrder(o);
        ob.matchOrders();
    }
}

// ---------- Metrics printer ----------
void metricsPrinter(OrderQueue &oq, OrderBook &ob, atomic<bool> &stopFlag) {
    using namespace chrono_literals;
    auto t0 = steady_clock::now();
    while(!stopFlag.load()) {
        this_thread::sleep_for(1s);
        auto t = steady_clock::now();
        double secs = chrono::duration_cast<chrono::duration<double>>(t - t0).count();

        long long trades = ob.totalTrades.load();
        long long bVol = ob.buyVolume.load();
        long long sVol = ob.sellVolume.load();
        long long processed = ob.processedOrders.load();
        long long avgLatencyNs = processed ? (ob.totalQueueLatencyNs.load() / processed) : 0;
        double avgLatencyMs = avgLatencyNs / 1e6;

        cout << "=== METRICS ===\n";
        cout << "Elapsed(s): " << fixed << setprecision(2) << secs << "\n";
        cout << "Trades: " << trades << " | BuyVol: " << bVol << " | SellVol: " << sVol << "\n";
        cout << "Queue size: " << oq.size() << " | Book Depth (B/S): "
             << ob.buyDepth() << "/" << ob.sellDepth() << "\n";
        cout << "Processed Orders: " << processed << " | Avg queue latency: "
             << avgLatencyMs << " ms\n";

        auto recent = ob.getRecentTrades(8);
        cout << "Recent trades (latest " << recent.size() << "):\n";
        for(auto &s : recent) cout << "  " << s << "\n";

        cout << "===============\n" << flush;
    }
}

// ---------- Signal handling ----------
atomic<bool> globalStop{false};
void handle_sigint(int) {
    globalStop.store(true);
}

// ---------- main ----------
int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // args: <num_producers> <orders_per_producer> <producer_delay_us> <engine_threads> <batch_size>
    int num_producers = 2;
    int orders_per_producer = 5000;
    int producer_delay_us = 20; // microseconds between orders (0 = burst)
    int engine_threads = 2;
    int batch_size = 16;

    if(argc >= 2) num_producers = stoi(argv[1]);
    if(argc >= 3) orders_per_producer = stoi(argv[2]);
    if(argc >= 4) producer_delay_us = stoi(argv[3]);
    if(argc >= 5) engine_threads = stoi(argv[4]);
    if(argc >= 6) batch_size = stoi(argv[5]);

    cout << "Starting headless trading engine with:\n";
    cout << " producers=" << num_producers
         << " orders_per_producer=" << orders_per_producer
         << " producer_delay_us=" << producer_delay_us
         << " engine_threads=" << engine_threads
         << " batch_size=" << batch_size << "\n";

    signal(SIGINT, handle_sigint);

    OrderQueue oq;
    OrderBook ob;
    atomic<bool> done(false);
    atomic<bool> stopFlag(false);

    // spawn producers
    vector<thread> producers;
    int idBase = 1;
    for(int i=0;i<num_producers;++i) {
        producers.emplace_back(marketProducer, std::ref(oq), orders_per_producer, idBase, producer_delay_us, std::ref(stopFlag));
        idBase += orders_per_producer;
    }

    // spawn engine workers
    vector<thread> engines;
    for(int i=0;i<engine_threads;++i) {
        engines.emplace_back(matchingEngineWorker, std::ref(oq), std::ref(ob), std::ref(done), batch_size, std::ref(stopFlag));
    }

    // metrics thread
    thread metrics(metricsPrinter, std::ref(oq), std::ref(ob), std::ref(stopFlag));

    // wait for producers
    for(auto &t : producers) {
        if(t.joinable()) t.join();
    }
    // tell engines we're done producing
    done.store(true);
    oq.notify_all();

    // wait for engines
    for(auto &t : engines) {
        if(t.joinable()) t.join();
    }

    // final metrics and shutdown
    stopFlag.store(true);
    if(metrics.joinable()) metrics.join();

    cout << "Final summary:\n";
    cout << "Total trades: " << ob.totalTrades.load() << "\n";
    cout << "BuyVol: " << ob.buyVolume.load() << " SellVol: " << ob.sellVolume.load() << "\n";
    cout << "Processed orders: " << ob.processedOrders.load() << "\n";
    cout << "Exiting.\n";
    return 0;
}

