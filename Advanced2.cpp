#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <csignal>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <random>
#include <array>
#include <optional>
#include <latch>
#include <barrier>

using namespace std;
using steady_clock = chrono::high_resolution_clock;
using ns = chrono::nanoseconds;

//  Lock-Free Ring Buffer 
template<typename T, size_t Capacity>
class LockFreeRingBuffer {
private:
    array<T, Capacity> buffer;
    atomic<size_t> head{0};
    atomic<size_t> tail{0};
    atomic<size_t> count{0};

public:
    bool try_push(T&& item) {
        size_t current_head = head.load(memory_order_acquire);
        size_t current_tail = tail.load(memory_order_acquire);
        
        if ((current_tail + 1) % Capacity == current_head) {
            return false; // Full
        }
        
        buffer[current_tail] = std::move(item);
        tail.store((current_tail + 1) % Capacity, memory_order_release);
        count.fetch_add(1, memory_order_relaxed);
        return true;
    }
    
    optional<T> try_pop() {
        size_t current_head = head.load(memory_order_acquire);
        size_t current_tail = tail.load(memory_order_acquire);
        
        if (current_head == current_tail) {
            return nullopt; // Empty
        }
        
        T item = std::move(buffer[current_head]);
        head.store((current_head + 1) % Capacity, memory_order_release);
        count.fetch_sub(1, memory_order_relaxed);
        return item;
    }
    
    size_t size() const {
        return count.load(memory_order_relaxed);
    }
    
    bool empty() const {
        return size() == 0;
    }
};

// Lock-Free Order 
struct alignas(64) Order {  // Cache line alignment
    int id;
    char side;
    double price;
    int quantity;
    steady_clock::time_point created;
    atomic<bool> processed{false};
};

//  Lock-Free Order Queue (Multiple Producers, Multiple Consumers)
class LockFreeOrderQueue {
private:
    static constexpr size_t BUFFER_SIZE = 65536; // Power of 2 for performance
    
    struct Slot {
        atomic<bool> ready{false};
        Order order;
    };
    
    vector<Slot> slots;
    atomic<size_t> producer_index{0};
    atomic<size_t> consumer_index{0};
    atomic<size_t> published_index{0};

public:
    LockFreeOrderQueue() : slots(BUFFER_SIZE) {}

    // Multiple producer support
    bool push(Order&& order) {
        size_t current_producer = producer_index.fetch_add(1, memory_order_acq_rel);
        size_t slot_index = current_producer % BUFFER_SIZE;
        
        Slot& slot = slots[slot_index];
        
        // Wait until slot is free
        while (slot.ready.load(memory_order_acquire)) {
            this_thread::yield();
        }
        
        slot.order = std::move(order);
        slot.ready.store(true, memory_order_release);
        
        // Update published index
        size_t expected = current_producer;
        while (!published_index.compare_exchange_weak(expected, current_producer + 1, 
                                                     memory_order_release, memory_order_relaxed)) {
            expected = current_producer;
            this_thread::yield();
        }
        
        return true;
    }
    
    // Multiple consumer support
    optional<Order> pop() {
        size_t current_consumer = consumer_index.load(memory_order_acquire);
        
        if (current_consumer >= published_index.load(memory_order_acquire)) {
            return nullopt; // No items available
        }
        
        size_t slot_index = current_consumer % BUFFER_SIZE;
        Slot& slot = slots[slot_index];
        
        if (!slot.ready.load(memory_order_acquire)) {
            return nullopt;
        }
        
        Order order = std::move(slot.order);
        slot.ready.store(false, memory_order_release);
        
        consumer_index.store(current_consumer + 1, memory_order_release);
        return order;
    }
    
    size_t size() const {
        return published_index.load(memory_order_acquire) - consumer_index.load(memory_order_acquire);
    }
    
    bool empty() const {
        return size() == 0;
    }
};

//  Lock-Free Order Book 
class LockFreeOrderBook {
private:
    struct OrderList {
        vector<Order> orders;
        atomic<bool> locked{false};
        
        bool try_lock() {
            bool expected = false;
            return locked.compare_exchange_strong(expected, true, memory_order_acquire);
        }
        
        void unlock() {
            locked.store(false, memory_order_release);
        }
    };
    
    OrderList buy_orders;
    OrderList sell_orders;
    
    // Lock-free statistics
    struct alignas(64) Statistics {
        atomic<long long> totalTrades{0};
        atomic<long long> buyVolume{0};
        atomic<long long> sellVolume{0};
        atomic<long long> processedOrders{0};
        atomic<long long> totalQueueLatencyNs{0};
    };
    
    Statistics stats;

    // Lock-free trade log
    static constexpr size_t TRADE_LOG_SIZE = 8192;
    array<atomic<string*>, TRADE_LOG_SIZE> trade_log;
    atomic<size_t> trade_log_index{0};

public:
    void addOrder(const Order& order) {
        OrderList& list = (order.side == 'B') ? buy_orders : sell_orders;
        
        // Spin until we get the lock
        while (!list.try_lock()) {
            this_thread::yield();
        }
        
        list.orders.push_back(order);
        stats.processedOrders.fetch_add(1, memory_order_relaxed);
        list.unlock();
    }
    
    void matchOrders() {
        // Try to lock both order books
        if (!buy_orders.try_lock() || !sell_orders.try_lock()) {
            if (buy_orders.locked.load()) buy_orders.unlock();
            return;
        }
        
        // Sort orders
        auto buy_cmp = [](const Order& a, const Order& b) {
            if (a.price != b.price) return a.price > b.price;
            return a.created < b.created;
        };
        
        auto sell_cmp = [](const Order& a, const Order& b) {
            if (a.price != b.price) return a.price < b.price;
            return a.created < b.created;
        };
        
        sort(buy_orders.orders.begin(), buy_orders.orders.end(), buy_cmp);
        sort(sell_orders.orders.begin(), sell_orders.orders.end(), sell_cmp);
        
        // Match orders
        while (!buy_orders.orders.empty() && !sell_orders.orders.empty() && 
               buy_orders.orders.front().price >= sell_orders.orders.front().price) {
            
            Order& buy = buy_orders.orders.front();
            Order& sell = sell_orders.orders.front();
            
            int tradeQty = min(buy.quantity, sell.quantity);
            double tradePrice = sell.price;
            
            // Update statistics (lock-free)
            stats.totalTrades.fetch_add(1, memory_order_relaxed);
            stats.buyVolume.fetch_add(tradeQty, memory_order_relaxed);
            stats.sellVolume.fetch_add(tradeQty, memory_order_relaxed);
            
            // Log trade (lock-free circular buffer)
            logTrade(buy.id, sell.id, tradeQty, tradePrice);
            
            // Update quantities
            buy.quantity -= tradeQty;
            sell.quantity -= tradeQty;
            
            if (buy.quantity == 0) {
                buy_orders.orders.erase(buy_orders.orders.begin());
            }
            if (sell.quantity == 0) {
                sell_orders.orders.erase(sell_orders.orders.begin());
            }
        }
        
        buy_orders.unlock();
        sell_orders.unlock();
    }
    
private:
    void logTrade(int buy_id, int sell_id, int quantity, double price) {
        ostringstream oss;
        auto t = chrono::duration_cast<chrono::milliseconds>(steady_clock::now().time_since_epoch()).count();
        oss << "T@" << t << " BUY_ID=" << buy_id << " SELL_ID=" << sell_id
            << " Q=" << quantity << " P=" << fixed << setprecision(2) << price;
        
        size_t index = trade_log_index.fetch_add(1, memory_order_acq_rel) % TRADE_LOG_SIZE;
        string* old_log = trade_log[index].exchange(new string(oss.str()), memory_order_release);
        delete old_log; // Safe because we own the old pointer
    }

public:
    // Lock-free statistics access
    long long getTotalTrades() const { return stats.totalTrades.load(memory_order_acquire); }
    long long getBuyVolume() const { return stats.buyVolume.load(memory_order_acquire); }
    long long getSellVolume() const { return stats.sellVolume.load(memory_order_acquire); }
    long long getProcessedOrders() const { return stats.processedOrders.load(memory_order_acquire); }
    
    void addQueueLatency(long long latency_ns) {
        stats.totalQueueLatencyNs.fetch_add(latency_ns, memory_order_relaxed);
    }
    
    double getAverageLatencyMs() const {
        long long processed = getProcessedOrders();
        long long total_latency = stats.totalQueueLatencyNs.load(memory_order_acquire);
        return processed ? (total_latency / 1e6) / processed : 0.0;
    }
    
    vector<string> getRecentTrades(size_t n = 20) {
        vector<string> recent;
        size_t current_index = trade_log_index.load(memory_order_acquire);
        size_t start_index = (current_index > n) ? current_index - n : 0;
        
        for (size_t i = start_index; i < current_index && i < TRADE_LOG_SIZE; ++i) {
            size_t index = i % TRADE_LOG_SIZE;
            string* log_entry = trade_log[index].load(memory_order_acquire);
            if (log_entry) {
                recent.push_back(*log_entry);
            }
        }
        return recent;
    }
    
    size_t getBuyDepth() const {
        while (!buy_orders.try_lock()) { this_thread::yield(); }
        size_t depth = buy_orders.orders.size();
        buy_orders.unlock();
        return depth;
    }
    
    size_t getSellDepth() const {
        while (!sell_orders.try_lock()) { this_thread::yield(); }
        size_t depth = sell_orders.orders.size();
        sell_orders.unlock();
        return depth;
    }
};

//   Advanced Producer with Batching 
void advancedProducer(LockFreeOrderQueue& oq, int numOrders, int idStart, 
                      atomic<bool>& stopFlag, int batchSize = 100) {
    vector<Order> batch;
    batch.reserve(batchSize);
    
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<> priceDist(95.0, 105.0);
    uniform_int_distribution<> qtyDist(100, 5000);
    uniform_int_distribution<> sideDist(0, 1);
    
    for (int i = 0; i < numOrders && !stopFlag.load(memory_order_acquire); i += batchSize) {
        batch.clear();
        
        // Create batch of orders
        int currentBatchSize = min(batchSize, numOrders - i);
        for (int j = 0; j < currentBatchSize; ++j) {
            Order order;
            order.id = idStart + i + j;
            order.side = sideDist(gen) ? 'B' : 'S';
            order.price = round(priceDist(gen) * 100) / 100;
            order.quantity = (qtyDist(gen) / 100) * 100;
            order.created = steady_clock::now();
            batch.push_back(std::move(order));
        }
        
        // Push batch to queue
        for (auto& order : batch) {
            while (!oq.push(std::move(order)) && !stopFlag.load(memory_order_acquire)) {
                this_thread::yield(); // Wait for space in queue
            }
        }
        
        // Adaptive delay based on queue performance
        this_thread::sleep_for(chrono::microseconds(10));
    }
}

//   Lock-Free Matching Engine Worker 
void lockFreeMatchingWorker(LockFreeOrderQueue& oq, LockFreeOrderBook& ob, 
                           atomic<bool>& stopFlag, int batchSize = 50) {
    vector<Order> batch;
    batch.reserve(batchSize);
    
    while (!stopFlag.load(memory_order_acquire) || !oq.empty()) {
        batch.clear();
        
        // Batch pop orders
        for (int i = 0; i < batchSize; ++i) {
            auto order_opt = oq.pop();
            if (!order_opt) break;
            
            Order& order = order_opt.value();
            auto now = steady_clock::now();
            auto queue_latency = chrono::duration_cast<ns>(now - order.created).count();
            
            ob.addQueueLatency(queue_latency);
            batch.push_back(std::move(order));
        }
        
        if (batch.empty()) {
            if (stopFlag.load(memory_order_acquire)) break;
            this_thread::sleep_for(chrono::microseconds(10));
            continue;
        }
        
        // Process batch
        for (const auto& order : batch) {
            ob.addOrder(order);
        }
        ob.matchOrders();
    }
}

//   High-Resolution Metrics Collector 
void advancedMetricsCollector(LockFreeOrderQueue& oq, LockFreeOrderBook& ob, 
                             atomic<bool>& stopFlag, ofstream& metricsFile) {
    using namespace chrono;
    
    auto start_time = steady_clock::now();
    auto last_sample = start_time;
    long long last_trades = 0;
    long long last_orders = 0;
    
    metricsFile << "timestamp,elapsed_ms,trades,trade_rate,orders,order_rate,"
                << "queue_size,buy_depth,sell_depth,avg_latency_ms" << endl;
    
    while (!stopFlag.load(memory_order_acquire)) {
        this_thread::sleep_for(100ms); // Higher frequency sampling
        
        auto now = steady_clock::now();
        auto elapsed_ms = duration_cast<milliseconds>(now - start_time).count();
        auto sample_elapsed_ms = duration_cast<milliseconds>(now - last_sample).count();
        
        long long current_trades = ob.getTotalTrades();
        long long current_orders = ob.getProcessedOrders();
        
        double trade_rate = sample_elapsed_ms > 0 ? 
            (current_trades - last_trades) / (sample_elapsed_ms / 1000.0) : 0;
        double order_rate = sample_elapsed_ms > 0 ? 
            (current_orders - last_orders) / (sample_elapsed_ms / 1000.0) : 0;
        
        // Write metrics
        metricsFile << duration_cast<milliseconds>(now.time_since_epoch()).count() << ","
                   << elapsed_ms << ","
                   << current_trades << ","
                   << trade_rate << ","
                   << current_orders << ","
                   << order_rate << ","
                   << oq.size() << ","
                   << ob.getBuyDepth() << ","
                   << ob.getSellDepth() << ","
                   << ob.getAverageLatencyMs() << endl;
        
        last_sample = now;
        last_trades = current_trades;
        last_orders = current_orders;
        
        // Console output every second
        static auto last_console_output = start_time;
        if (duration_cast<seconds>(now - last_console_output).count() >= 1) {
            cout << "=== LOCK-FREE METRICS ===" << endl;
            cout << "Trades: " << current_trades << " (" << trade_rate << "/sec)" << endl;
            cout << "Orders: " << current_orders << " (" << order_rate << "/sec)" << endl;
            cout << "Queue: " << oq.size() << " | Latency: " << ob.getAverageLatencyMs() << "ms" << endl;
            cout << "========================" << endl;
            last_console_output = now;
        }
    }
}

//   Signal Handling 
atomic<bool> global_stop{false};
void handle_signal(int) {
    global_stop.store(true, memory_order_release);
}

//  Main with Advanced Configuration 
int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    
    // Advanced configuration
    int num_producers = 4;
    int orders_per_producer = 100000;
    int engine_threads = 8;
    int producer_batch_size = 200;
    int consumer_batch_size = 100;
    
    if (argc >= 2) num_producers = stoi(argv[1]);
    if (argc >= 3) orders_per_producer = stoi(argv[2]);
    if (argc >= 4) engine_threads = stoi(argv[3]);
    if (argc >= 5) producer_batch_size = stoi(argv[4]);
    if (argc >= 6) consumer_batch_size = stoi(argv[5]);
    
    cout << "=== LOCK-FREE TRADING ENGINE ===" << endl;
    cout << "Producers: " << num_producers << " x " << orders_per_producer << " orders" << endl;
    cout << "Engine Threads: " << engine_threads << endl;
    cout << "Producer Batch: " << producer_batch_size << endl;
    cout << "Consumer Batch: " << consumer_batch_size << endl;
    cout << "Total Orders: " << (num_producers * orders_per_producer) << endl;
    
    signal(SIGINT, handle_signal);
    
    // Initialize components
    LockFreeOrderQueue order_queue;
    LockFreeOrderBook order_book;
    
    // Open metrics file
    ofstream metrics_file("lockfree_metrics.csv");
    if (!metrics_file.is_open()) {
        cerr << "Failed to open metrics file" << endl;
        return 1;
    }
    
    // Start producers
    vector<thread> producers;
    int id_base = 1;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back(advancedProducer, ref(order_queue), orders_per_producer, 
                              id_base, ref(global_stop), producer_batch_size);
        id_base += orders_per_producer;
    }
    
    // Start matching engines
    vector<thread> engines;
    for (int i = 0; i < engine_threads; ++i) {
        engines.emplace_back(lockFreeMatchingWorker, ref(order_queue), ref(order_book), 
                           ref(global_stop), consumer_batch_size);
    }
    
    // Start metrics collector
    thread metrics_collector(advancedMetricsCollector, ref(order_queue), ref(order_book), 
                           ref(global_stop), ref(metrics_file));
    
    // Wait for producers
    for (auto& producer : producers) {
        producer.join();
    }
    
    // Signal stop and wait for engines
    global_stop.store(true, memory_order_release);
    for (auto& engine : engines) {
        engine.join();
    }
    
    // Stop metrics
    metrics_collector.join();
    metrics_file.close();
    
    // Final statistics
    cout << "\n=== FINAL LOCK-FREE STATISTICS ===" << endl;
    cout << "Total Trades: " << order_book.getTotalTrades() << endl;
    cout << "Total Orders Processed: " << order_book.getProcessedOrders() << endl;
    cout << "Buy Volume: " << order_book.getBuyVolume() << " shares" << endl;
    cout << "Sell Volume: " << order_book.getSellVolume() << " shares" << endl;
    cout << "Final Queue Size: " << order_queue.size() << endl;
    cout << "Final Book Depth - Buys: " << order_book.getBuyDepth() 
         << ", Sells: " << order_book.getSellDepth() << endl;
    cout << "Average Latency: " << order_book.getAverageLatencyMs() << " ms" << endl;
    
    // Show recent trades
    auto recent_trades = order_book.getRecentTrades(5);
    cout << "Recent Trades:" << endl;
    for (const auto& trade : recent_trades) {
        cout << "  " << trade << endl;
    }
    
    return 0;
}
