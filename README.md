Trading Engine
A high-performance, multi-threaded trading engine simulation built in C++ that handles real-time order matching with multi-threaded processing capabilities.

Overview

This project implements a complete trading engine simulation that demonstrates:
Multi-threaded order processing
Real-time order matching with price-time priority
Thread-safe data structures
Performance monitoring and metrics
Configurable trading parameters


Architecture

<img width="686" height="586" alt="Screenshot 2025-10-18 020401" src="https://github.com/user-attachments/assets/75a5f503-be92-4a63-ae69-e051a360303c" />


Technical Challenges & Solutions
Challenge 1: Thread Synchronization in Order Queue
Problem: Multiple producer and consumer threads accessing the order queue simultaneously caused data races and inconsistent state.

Solution: Implemented a thread-safe queue with mutex and condition variables:

cpp
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
        cv.wait_for(lock, chrono::milliseconds(50), [&]{ 
            return !q.empty() || done.load(); 
        });
        // Batch processing logic
    }
};


Challenge 2: Efficient Producer-Consumer Coordination
Problem: Traditional approaches either busy-wait (wasting CPU) or have poor latency when new orders arrive.

Solution: Used condition variables with timeout for optimal responsiveness:

cpp
cv.wait_for(lock, chrono::milliseconds(50), [&]{ 
    return !q.empty() || done.load(); 
});
This provides:

Immediate wakeup when orders arrive

Periodic checks for shutdown signals

No CPU waste during idle periods



Challenge 3: Fair Order Matching Algorithm
Problem: Needed to implement price-time priority matching while maintaining thread safety and performance.

Solution: Developed a dual-sorting approach with fine-grained locking:

cpp
void matchOrders() {
    lock_guard<mutex> lock(ob_mtx);
    
    // Price-time priority: higher buys first, lower sells first
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
    
    // Cross matching when prices overlap
    while(!buys.empty() && !sells.empty() && 
          buys.front().price >= sells.front().price) {
        // Execute trade logic
    }
}





Challenge 4: Memory Management and Resource Limits
Problem: Unbounded growth of trade logs could exhaust memory during long runs.

Solution: Implemented fixed-size circular buffer for trade logs:

cpp
deque<string> tradeLog;
size_t tradeLogLimit = 2000;

// In logging logic:
tradeLog.push_back(oss.str());
if(tradeLog.size() > tradeLogLimit) tradeLog.pop_front();





Challenge 5: Performance Under High Load
Problem: Processing orders one-by-one caused excessive locking overhead.

Solution: Implemented batch processing to amortize locking costs:

cpp
vector<Order> batch;
batch.reserve(batchSize);

size_t got = oq.pop_batch(batch, batchSize, done);
if(got > 0) {
    // Process entire batch with single lock acquisition
    for(const auto &o: batch) ob.addOrder(o);
    ob.matchOrders();
}




Challenge 6: Graceful Shutdown Handling
Problem: Threads needed to cleanly exit on signal interrupts without data loss.

Solution: Atomic flags with coordinated shutdown sequence:

cpp
atomic<bool> globalStop{false};
atomic<bool> done(false);

void handle_sigint(int) {
    globalStop.store(true);
}

// In worker threads:
while(!done.load() || !oq.empty()) {
    // Check shutdown flag periodically
}
Key Features
Multi-threaded Architecture: Separate producer, matching engine, and metrics threads

Batch Processing: Configurable batch sizes (default: 16 orders/batch)

Thread-safe Data Structures: Lock-based queues with condition variables

Real-time Metrics: Latency tracking, volume statistics, trade logging

Configurable Parameters: Tunable for different performance characteristics

Usage
Compilation
bash
g++ -std=c++17 -pthread -O2 trading_engine.cpp -o trading_engine
Running
bash
# Default configuration
./trading_engine

# Custom configuration
./trading_engine <num_producers> <orders_per_producer> <producer_delay_us> <engine_threads> <batch_size>

# High-performance example
./trading_engine 4 10000 10 4 32
Configuration Parameters
Parameter	Description	Default
num_producers	Order producer threads	2
orders_per_producer	Orders per producer	5000
producer_delay_us	Delay between orders (0=burst)	20
engine_threads	Matching engine threads	2
batch_size	Orders processed per batch	16
Performance Metrics
The system tracks and displays:

Order queue latency statistics

Trade execution volumes

Order book depth

Processing throughput

Recent trade history

Limitations & Future Enhancements
Basic price-time matching only

In-memory storage (no persistence)

Market orders only (no limit orders)

Simulated data input (no network interface)





This implementation provides a solid foundation for understanding high-frequency trading systems and can be extended with additional order types, persistence layers, and network interfaces.




