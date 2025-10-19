## Trading Engine Program 
---------------------------------------------------------------------------------------
--------------------------------------------------------------------------------------

This is a multi-threaded trading engine simulator that mimics how real stock exchanges work. It processes buy/sell orders, matches them to create trades, and provides real-time statistics.

Think of it like a virtual stock market where:
(i)   Producers create buy/sell orders
(ii)  Matching Engine finds matching buy/sell orders to execute trades
(iii) Metrics show what's happening in real-time

-------------------------------------------------------------------------------------

## (1) Order Structure - What is a Trade Order

```cpp
struct Order {
    int id;          // Unique order number (like ticket number)
    char side;       // 'B' for BUY or 'S' for SELL
    double price;    // Price per share (e.g., $100.50)
    int quantity;    // How many shares to buy/sell
    steady_clock::time_point created; // When order was created
};
```

Example Orders:

(i)  {id: 1, side: 'B', price: 100.50, quantity: 10} → "Buy 10 shares at $100.50 each"
(ii) {id: 2, side: 'S', price: 100.00, quantity: 5} → "Sell 5 shares at $100.00 each"

---------------------------------------------------------------------------------------
## (2) OrderQueue Class - The Waiting Line for Orders

```cpp
class OrderQueue {
    // Like a supermarket checkout line for orders
    // Thread-safe: multiple threads can use it without problems
};
```
Stores orders in a queue (first-in, first-out)
Handles multiple threads safely
Notifies workers when new orders arrive

--------------------------------------------------------------------------------------
## (3) OrderBook Class - The Trading Board
```cpp
class OrderBook {
    vector<Order> buys;   // All BUY orders waiting
    vector<Order> sells;  // All SELL orders waiting
    // Plus statistics and trade history
};
```

What it does:
(i)   Stores all active buy/sell orders
(ii)  Matches compatible orders (when buy price ≥ sell price)
(iii) Keeps track of trades and statistics

---------------------------------------------------------------------------------------
## (4) Main Functions - The Workers
## Market Producer - Order Creator

```cpp
void marketProducer(...)
```
Creates fake buy/sell orders for testing
Can control how fast orders are created

## Matching Engine - The Brain
```cpp
void matchingEngineWorker(...)
```
Takes orders from queue
Adds them to the order book
Finds and executes matching trades

## Metrics Printer - The Reporter
```cpp
void metricsPrinter(...)
```
Shows real-time statistics every second
Displays recent trades and performance metrics

---------------------------------------------------------------------------------------

## How It All Works Together

<img width="780" height="419" alt="Screenshot 2025-10-20 031441" src="https://github.com/user-attachments/assets/b9ecad53-ea16-421f-a01d-635e41d38269" />

