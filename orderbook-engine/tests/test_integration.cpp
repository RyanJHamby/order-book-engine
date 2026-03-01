#include <gtest/gtest.h>
#include "../include/orderbook.hpp"
#include "../include/order_queue.hpp"
#include "../include/memory_pool.hpp"
#include <thread>
#include <vector>
#include <random>

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ob = OrderBook();
    }
    void TearDown() override {}

    OrderBook ob;
};

TEST_F(IntegrationTest, OrderBookWithMemoryPool) {
    ThreadLocalPool<Order> pool(1000);

    Order* buy_order = pool.allocate();
    buy_order->id = 1;
    buy_order->type = OrderType::BUY;
    buy_order->price = 100.0;
    buy_order->quantity = 10;

    Order* sell_order = pool.allocate();
    sell_order->id = 2;
    sell_order->type = OrderType::SELL;
    sell_order->price = 200.0;
    sell_order->quantity = 20;

    ob.add_order(*buy_order);
    ob.add_order(*sell_order);

    // Prices don't cross: buy@100 < sell@200.
    EXPECT_EQ(ob.get_fills().size(), 0u);
    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_EQ(ob.ask_count(), 1u);
}

TEST_F(IntegrationTest, OrderBookWithQueue) {
    LockFreeQueue<Order, 128> queue;

    for (int i = 0; i < 50; ++i) {
        Order order{static_cast<uint64_t>(i),
                   (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                   100.0 + i,
                   static_cast<uint32_t>(i + 1)};
        EXPECT_TRUE(queue.push(order));
    }

    Order popped{0, OrderType::BUY, 0.0, 0};
    int count = 0;
    while (queue.pop(popped) && count < 50) {
        ob.add_order(popped);
        count++;
    }

    EXPECT_EQ(count, 50);
    EXPECT_GT(ob.get_fills().size(), 0u);
}

TEST_F(IntegrationTest, FullPipeline) {
    LockFreeQueue<Order, 1024> queue;
    ThreadLocalPool<Order> pool(1000);

    const int num_orders = 100;
    for (int i = 0; i < num_orders; ++i) {
        Order* order = pool.allocate();
        order->id = i;
        order->type = (i % 2 == 0) ? OrderType::BUY : OrderType::SELL;
        order->price = 100.0 + (i % 50);
        order->quantity = (i % 20) + 1;
        EXPECT_TRUE(queue.push(*order));
    }

    Order popped{0, OrderType::BUY, 0.0, 0};
    int processed = 0;
    while (queue.pop(popped)) {
        ob.add_order(popped);
        processed++;
    }

    EXPECT_EQ(processed, num_orders);
    EXPECT_GT(ob.get_fills().size(), 0u);
}

TEST_F(IntegrationTest, ConcurrentQueueToSingleMatchingThread) {
    // Correct architecture: SPSC queue per producer, one matching thread.
    LockFreeQueue<Order, 1024> queue;
    const int num_orders = 100;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < num_orders; ++i) {
            Order order{static_cast<uint64_t>(i),
                       (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                       100.0 + (i % 50),
                       static_cast<uint32_t>((i % 20) + 1)};
            while (!queue.push(order)) std::this_thread::yield();
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
