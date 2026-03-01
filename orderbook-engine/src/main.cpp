// main.cpp
// Demonstrates the full pipeline: pool allocation -> SPSC queue -> matching engine.
#include "orderbook.hpp"
#include "order_queue.hpp"
#include "memory_pool.hpp"
#include <iostream>
#include <thread>
#include <atomic>

int main() {
    constexpr std::size_t Q_SIZE = 8192;
    LockFreeQueue<Order, Q_SIZE> queue;
    OrderBook book;
    std::atomic<bool> producer_done{false};

    // Producer: allocate orders from thread-local pool, push to queue.
    std::thread producer([&]() {
        auto& pool = get_thread_local_pool<Order>(4096);

        Order* o1 = pool.allocate();
        *o1 = {1, OrderType::SELL, 101.0, 50};
        while (!queue.push(*o1)) std::this_thread::yield();

        Order* o2 = pool.allocate();
        *o2 = {2, OrderType::SELL, 102.0, 30};
        while (!queue.push(*o2)) std::this_thread::yield();

        Order* o3 = pool.allocate();
        *o3 = {3, OrderType::SELL, 100.5, 20};
        while (!queue.push(*o3)) std::this_thread::yield();

        // Aggressive buy sweeps through price levels.
        Order* o4 = pool.allocate();
        *o4 = {4, OrderType::BUY, 102.0, 60};
        while (!queue.push(*o4)) std::this_thread::yield();

        // Order to cancel.
        Order* o5 = pool.allocate();
        *o5 = {5, OrderType::SELL, 105.0, 10};
        while (!queue.push(*o5)) std::this_thread::yield();

        producer_done.store(true, std::memory_order_release);
    });

    // Consumer / matching thread: drain queue into order book.
    Order incoming{0, OrderType::BUY, 0.0, 0};
    while (true) {
        if (queue.pop(incoming)) {
            book.add_order(incoming);
        } else if (producer_done.load(std::memory_order_acquire)) {
            while (queue.pop(incoming)) book.add_order(incoming);
            break;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    std::cout << "Fills:\n";
    for (const auto& f : book.get_fills()) {
        std::cout << "  buy=" << f.buy_order_id
                  << " sell=" << f.sell_order_id
                  << " @ " << f.price
                  << " qty=" << f.quantity << "\n";
    }

    std::cout << "Resting bids: " << book.bid_count()
              << "  Resting asks: " << book.ask_count() << "\n";

    book.cancel_order(5);
    std::cout << "After cancel(5) asks: " << book.ask_count() << "\n";

    return 0;
}
