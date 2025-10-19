
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
#include <fstream>
#include <random>

using namespace std;
using steady_clock = chrono::high_resolution_clock;
using ns = chrono::nanoseconds;

// Order 
struct Order {
    int id;
    char side; // 'B' or 'S'
    double price;
    int quantity;
    steady_clock::time_point created;
};

// Thread-safe blocking queue
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

//  OrderBook 
class OrderBook {
    vector<Order> buys;
    vector<Order> sells;
    mutable mutex ob_mtx;

public:
    atomic<long long> totalTrades{0};
    atomic<long long> buyVolume{0};
    atomic<long long> sellVolume{0};
    atomic<long long> processedOrders{0};
    atomic<long long> totalQueueLatencyNs{0};

    mutex log_mtx;
    deque<string> tradeLog;
    size_t tradeLogLimit = 5000; // Increased for large files

    void addOrder(const Order &o) {
        lock_guard<mutex> lock(ob_mtx);
        if(o.side == 'B') buys.push_back(o);
        else sells.push_back(o);
        processedOrders.fetch_add(1, memory_order_relaxed);
    }

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
            double tradePrice = s.price;

            totalTrades.fetch_add(1, memory_order_relaxed);
            buyVolume.fetch_add(tradeQty, memory_order_relaxed);
            sellVolume.fetch_add(tradeQty, memory_order_relaxed);

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

// File-based Producer 
void fileProducer(OrderQueue &oq, const string& filename, atomic<bool> &stopFlag) {
    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Cannot open input file: " << filename << endl;
        return;
    }

    string line;
    int orderId = 1;
    while (getline(file, line) && !stopFlag.load()) {
        stringstream ss(line);
        string sideStr, idStr, priceStr, qtyStr;
        
        getline(ss, sideStr, ',');
        getline(ss, idStr, ',');
        getline(ss, priceStr, ',');
        getline(ss, qtyStr, ',');

        Order o;
        o.id = orderId++;
        o.side = sideStr[0];
        o.price = stod(priceStr);
        o.quantity = stoi(qtyStr);
        o.created = steady_clock::now();

        oq.push(std::move(o));
        
        // Small delay to simulate real-time
        this_thread::sleep_for(chrono::microseconds(10));
    }
    file.close();
}

//  Matching Engine -
void matchingEngineWorker(OrderQueue &oq, OrderBook &ob, atomic<bool> &done,
                          int batchSize, atomic<bool> &stopFlag) {
    vector<Order> batch;
    batch.reserve(batchSize);

    while(!done.load() || !oq.empty()) {
        batch.clear();
        size_t got = oq.pop_batch(batch, batchSize, done);
        if(got == 0) {
            if(done.load()) break;
            continue;
        }

        auto now = steady_clock::now();
        long long localLatencyNs = 0;
        for(auto &o : batch) {
            auto qlat = chrono::duration_cast<ns>(now - o.created).count();
            localLatencyNs += qlat;
            ob.totalQueueLatencyNs.fetch_add(qlat, memory_order_relaxed);
        }

        for(const auto &o: batch) ob.addOrder(o);
        ob.matchOrders();
    }
}

//  Metrics with File Output
void metricsPrinter(OrderQueue &oq, OrderBook &ob, atomic<bool> &stopFlag, ofstream& outFile) {
    using namespace chrono_literals;
    auto t0 = steady_clock::now();
    
    outFile << "=== TRADING ENGINE STARTED ===" << endl;
    outFile << "Timestamp,ElapsedSec,Trades,BuyVol,SellVol,QueueSize,BookBuy,BookSell,ProcessedOrders,AvgLatencyMs" << endl;

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

        // Write to file (CSV format)
        outFile << chrono::duration_cast<chrono::milliseconds>(t.time_since_epoch()).count() << ","
                << fixed << setprecision(2) << secs << ","
                << trades << "," << bVol << "," << sVol << ","
                << oq.size() << "," << ob.buyDepth() << "," << ob.sellDepth() << ","
                << processed << "," << avgLatencyMs << endl;

        // Also print to console
        cout << "=== METRICS [Time: " << secs << "s] ===" << endl;
        cout << "Trades: " << trades << " | BuyVol: " << bVol << " | SellVol: " << sVol << endl;
        cout << "Queue: " << oq.size() << " | Book: " << ob.buyDepth() << "B/" << ob.sellDepth() << "S" << endl;
        cout << "Processed: " << processed << " | Avg Latency: " << avgLatencyMs << " ms" << endl;
    }
}

//  Signal Handling 
atomic<bool> globalStop{false};
void handle_sigint(int) {
    globalStop.store(true);
}

//  Order Generator for Large Files 
void generateLargeOrderFile(const string& filename, int numOrders) {
    ofstream file(filename);
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<> priceDist(95.0, 105.0);
    uniform_int_distribution<> qtyDist(100, 5000);
    uniform_int_distribution<> sideDist(0, 1);
    
    file << "Side,ID,Price,Quantity" << endl;
    for (int i = 1; i <= numOrders; ++i) {
        char side = sideDist(gen) ? 'B' : 'S';
        double price = round(priceDist(gen) * 100) / 100; // Round to 2 decimal places
        int quantity = (qtyDist(gen) / 100) * 100; // Round to nearest 100
        
        file << side << "," << i << "," << fixed << setprecision(2) << price 
             << "," << quantity << endl;
    }
    file.close();
    cout << "Generated " << numOrders << " orders in " << filename << endl;
}

//  Main 
int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Default: generate and process 10,000 orders
    int num_orders = 10000;
    string input_file = "large_orders_input.txt";
    string output_file = "large_trading_output.csv";
    int engine_threads = 4;
    int batch_size = 16;

    if(argc >= 2) num_orders = stoi(argv[1]);
    if(argc >= 3) input_file = argv[2];
    if(argc >= 4) output_file = argv[3];
    if(argc >= 5) engine_threads = stoi(argv[4]);
    if(argc >= 6) batch_size = stoi(argv[5]);

    cout << "Generating " << num_orders << " orders..." << endl;
    generateLargeOrderFile(input_file, num_orders);

    cout << "Starting trading engine with file I/O..." << endl;
    cout << "Input: " << input_file << endl;
    cout << "Output: " << output_file << endl;
    cout << "Engine threads: " << engine_threads << endl;
    cout << "Batch size: " << batch_size << endl;

    signal(SIGINT, handle_sigint);

    // Open output file
    ofstream outFile(output_file);
    if (!outFile.is_open()) {
        cerr << "Cannot open output file: " << output_file << endl;
        return 1;
    }

    OrderQueue oq;
    OrderBook ob;
    atomic<bool> done(false);
    atomic<bool> stopFlag(false);

    // Start file producer
    thread producer(fileProducer, std::ref(oq), input_file, std::ref(stopFlag));

    // Start engine workers
    vector<thread> engines;
    for(int i = 0; i < engine_threads; ++i) {
        engines.emplace_back(matchingEngineWorker, std::ref(oq), std::ref(ob), 
                           std::ref(done), batch_size, std::ref(stopFlag));
    }

    // Start metrics with file output
    thread metrics(metricsPrinter, std::ref(oq), std::ref(ob), 
                   std::ref(stopFlag), std::ref(outFile));

    // Wait for producer to finish
    producer.join();
    done.store(true);
    oq.notify_all();

    // Wait for engines
    for(auto &t : engines) {
        if(t.joinable()) t.join();
    }

    // Final statistics
    stopFlag.store(true);
    metrics.join();

    // Write final summary
    outFile << "\n=== FINAL SUMMARY ===" << endl;
    outFile << "Total Orders Processed: " << ob.processedOrders.load() << endl;
    outFile << "Total Trades: " << ob.totalTrades.load() << endl;
    outFile << "Total Buy Volume: " << ob.buyVolume.load() << " shares" << endl;
    outFile << "Total Sell Volume: " << ob.sellVolume.load() << " shares" << endl;
    outFile << "Final Book Depth: " << ob.buyDepth() << " Buys, " << ob.sellDepth() << " Sells" << endl;

    outFile.close();

    cout << "\n=== SIMULATION COMPLETE ===" << endl;
    cout << "Processed " << ob.processedOrders.load() << " orders" << endl;
    cout << "Executed " << ob.totalTrades.load() << " trades" << endl;
    cout << "Output written to: " << output_file << endl;

    return 0;
}
