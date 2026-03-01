#include <gtest/gtest.h>
#include "../include/orderbook.hpp"
#include "../include/order_queue.hpp"
#include "../include/memory_pool.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <random>

// The matching engine is single-threaded by design. Orders are submitted
// through lock-free queues and consumed by a dedicated matching thread.
// These tests validate that architecture.

class ConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ob = OrderBook();
    }
    void TearDown() override {}

    OrderBook ob;
};

TEST_F(ConcurrencyTest, SPSCQueueProducerConsumer) {
    // Single producer, single consumer — the correct SPSC usage.
    LockFreeQueue<Order, 4096> queue;
    const int num_orders = 2000;
    std::atomic<bool> producer_done{false};
    std::atomic<int> consumed{0};

    // Producer thread.
    std::thread producer([&]() {
        for (int i = 0; i < num_orders; ++i) {
            Order order{static_cast<uint64_t>(i),
                       (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                       100.0 + (i % 50),
                       static_cast<uint32_t>((i % 20) + 1)};
            while (!queue.push(order)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer / matching thread.
    std::thread consumer([&]() {
        Order order{0, OrderType::BUY, 0.0, 0};
        while (true) {
            if (queue.pop(order)) {
                ob.add_order(order);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (producer_done.load(std::memory_order_acquire)) {
                // Drain remaining.
                while (queue.pop(order)) {
                    ob.add_order(order);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), num_orders);
    EXPECT_GT(ob.get_fills().size(), 0u);
}

TEST_F(ConcurrencyTest, MultipleSPSCQueuesIntoMatchingEngine) {
    // Multiple producers each with their own SPSC queue feeding one matching thread.
    const int num_producers = 4;
    const int orders_per_producer = 500;
    constexpr std::size_t Q_SIZE = 1024;

    std::vector<LockFreeQueue<Order, Q_SIZE>> queues(num_producers);
    std::vector<std::atomic<bool>> done_flags(num_producers);
    for (auto& f : done_flags) f.store(false);

    // Producer threads — each owns its own queue.
    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < orders_per_producer; ++i) {
                Order order{static_cast<uint64_t>(p * orders_per_producer + i),
                           (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                           100.0 + (i % 50),
                           static_cast<uint32_t>((i % 20) + 1)};
                while (!queues[p].push(order)) {
                    std::this_thread::yield();
                }
            }
            done_flags[p].store(true, std::memory_order_release);
        });
    }

    // Single matching thread round-robins across queues.
    int total_consumed = 0;
    std::thread matcher([&]() {
        Order order{0, OrderType::BUY, 0.0, 0};
        int producers_done = 0;
        while (producers_done < num_producers) {
            producers_done = 0;
            for (int p = 0; p < num_producers; ++p) {
                if (queues[p].pop(order)) {
                    ob.add_order(order);
                    total_consumed++;
                }
                if (done_flags[p].load(std::memory_order_acquire)) {
                    producers_done++;
                }
            }
        }
        // Final drain.
        for (int p = 0; p < num_producers; ++p) {
            while (queues[p].pop(order)) {
                ob.add_order(order);
                total_consumed++;
            }
        }
    });

    for (auto& t : producers) t.join();
    matcher.join();

    EXPECT_EQ(total_consumed, num_producers * orders_per_producer);
    EXPECT_GT(ob.get_fills().size(), 0u);
}

TEST_F(ConcurrencyTest, ConcurrentQueueOperations) {
    // Validates SPSC queue correctness under contention.
    LockFreeQueue<Order, 4096> queue;
    const int num_ops = 2000;
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};

    std::thread producer([&]() {
        for (int i = 0; i < num_ops; ++i) {
            Order order{static_cast<uint64_t>(i), OrderType::BUY, 100.0 + i, 1};
            while (!queue.push(order)) std::this_thread::yield();
            push_count.fetch_add(1);
        }
    });

    std::thread consumer([&]() {
        Order order{0, OrderType::BUY, 0.0, 0};
        while (pop_count.load() < num_ops) {
            if (queue.pop(order)) {
                pop_count.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(push_count.load(), num_ops);
    EXPECT_EQ(pop_count.load(), num_ops);
}

TEST_F(ConcurrencyTest, ConcurrentMemoryPoolAllocation) {
    // Each thread gets its own thread-local pool — no shared state.
    const int num_threads = 4;
    const int allocations_per_thread = 1000;
    std::atomic<int> total_allocations{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&total_allocations, t, allocations_per_thread]() {
            ThreadLocalPool<Order> pool(allocations_per_thread);
            std::vector<Order*> local_allocs;
            local_allocs.reserve(allocations_per_thread);

            for (int i = 0; i < allocations_per_thread; ++i) {
                Order* order = pool.allocate();
                EXPECT_NE(order, nullptr);
                order->id = t * allocations_per_thread + i;
                order->type = OrderType::BUY;
                order->price = 100.0 + i;
                order->quantity = i + 1;
                local_allocs.push_back(order);
                total_allocations.fetch_add(1);
            }

            // All allocations unique within this thread.
            for (std::size_t i = 0; i < local_allocs.size(); ++i) {
                for (std::size_t j = i + 1; j < local_allocs.size(); ++j) {
                    EXPECT_NE(local_allocs[i], local_allocs[j]);
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(total_allocations.load(), num_threads * allocations_per_thread);
}

TEST_F(ConcurrencyTest, FullPipelineEndToEnd) {
    // Full pipeline: pool allocation -> queue -> matching engine.
    LockFreeQueue<Order, 4096> queue;
    const int num_orders = 1000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        ThreadLocalPool<Order> pool(num_orders);
        for (int i = 0; i < num_orders; ++i) {
            Order* o = pool.allocate();
            o->id = i;
            o->type = (i % 2 == 0) ? OrderType::BUY : OrderType::SELL;
            o->price = 100.0 + (i % 50);
            o->quantity = (i % 20) + 1;
            while (!queue.push(*o)) std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });

    int consumed = 0;
    std::thread matcher([&]() {
        Order order{0, OrderType::BUY, 0.0, 0};
        while (true) {
            if (queue.pop(order)) {
                ob.add_order(order);
                consumed++;
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (queue.pop(order)) {
                    ob.add_order(order);
                    consumed++;
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    matcher.join();

    EXPECT_EQ(consumed, num_orders);
    EXPECT_GT(ob.get_fills().size(), 0u);
}
