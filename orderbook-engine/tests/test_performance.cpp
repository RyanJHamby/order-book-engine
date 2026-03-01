#include <gtest/gtest.h>
#include "../include/orderbook.hpp"
#include "../include/order_queue.hpp"
#include "../include/memory_pool.hpp"
#include <chrono>
#include <random>

class PerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ob = OrderBook();
        rng.seed(42);
    }
    void TearDown() override {}

    OrderBook ob;
    std::mt19937 rng;
};

TEST_F(PerformanceTest, OrderBookAddPerformance) {
    const int num_orders = 100000;
    std::uniform_real_distribution<double> price_dist(100, 200);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_orders; ++i) {
        Order order{static_cast<uint64_t>(i),
                   (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                   price_dist(rng),
                   static_cast<uint32_t>(qty_dist(rng))};
        ob.add_order(order);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double orders_per_second = static_cast<double>(num_orders) / duration.count() * 1'000'000;

    std::cout << "OrderBook Add+Match Performance: " << orders_per_second << " orders/sec" << std::endl;
    std::cout << "  Fills generated: " << ob.get_fills().size() << std::endl;

    EXPECT_GT(orders_per_second, 100000);
}

TEST_F(PerformanceTest, OrderBookMatchPerformance) {
    const int num_orders = 50000;
    std::uniform_real_distribution<double> price_dist(100, 200);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_orders; ++i) {
        Order order{static_cast<uint64_t>(i),
                   (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                   price_dist(rng),
                   static_cast<uint32_t>(qty_dist(rng))};
        ob.add_order(order);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "OrderBook Match Performance: " << duration.count() << " us for "
              << num_orders << " orders (" << ob.get_fills().size() << " fills)" << std::endl;

    EXPECT_LT(duration.count(), 500000); // < 500ms for 50K with real matching
}

TEST_F(PerformanceTest, LockFreeQueueThroughput) {
    LockFreeQueue<Order, 100000> queue;
    const int num_orders = 50000;
    std::uniform_real_distribution<double> price_dist(100, 200);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    std::vector<Order> orders;
    orders.reserve(num_orders);
    for (int i = 0; i < num_orders; ++i) {
        orders.push_back({static_cast<uint64_t>(i),
                         (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                         price_dist(rng),
                         static_cast<uint32_t>(qty_dist(rng))});
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& order : orders) {
        EXPECT_TRUE(queue.push(order));
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto push_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double push_rate = static_cast<double>(num_orders) / push_duration.count() * 1'000'000;

    start = std::chrono::high_resolution_clock::now();
    Order popped{0, OrderType::BUY, 0.0, 0};
    int popped_count = 0;
    while (queue.pop(popped)) popped_count++;
    end = std::chrono::high_resolution_clock::now();

    auto pop_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    double pop_rate = static_cast<double>(popped_count) / pop_duration.count() * 1'000'000;

    std::cout << "LockFreeQueue Push Rate: " << push_rate << " orders/sec" << std::endl;
    std::cout << "LockFreeQueue Pop Rate: " << pop_rate << " orders/sec" << std::endl;

    EXPECT_EQ(popped_count, num_orders);
    EXPECT_GT(push_rate, 1000000);
    EXPECT_GT(pop_rate, 1000000);
}

TEST_F(PerformanceTest, MemoryPoolAllocationSpeed) {
    ThreadLocalPool<Order> pool(100000);
    const int num_allocations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<Order*> allocations;
    allocations.reserve(num_allocations);
    for (int i = 0; i < num_allocations; ++i) {
        Order* order = pool.allocate();
        EXPECT_NE(order, nullptr);
        allocations.push_back(order);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double alloc_rate = static_cast<double>(num_allocations) / duration.count() * 1'000'000;

    std::cout << "MemoryPool Allocation Rate: " << alloc_rate << " allocs/sec" << std::endl;
    EXPECT_GT(alloc_rate, 1000000);
}

TEST_F(PerformanceTest, EndToEndLatency) {
    const int num_orders = 10000;
    std::uniform_real_distribution<double> price_dist(100, 200);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_orders; ++i) {
        Order order{static_cast<uint64_t>(i),
                   (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                   price_dist(rng),
                   static_cast<uint32_t>(qty_dist(rng))};
        ob.add_order(order);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double avg_latency_ns = static_cast<double>(duration.count()) / num_orders;
    double avg_latency_us = avg_latency_ns / 1000.0;

    std::cout << "End-to-End Average Latency: " << avg_latency_us << " us per order ("
              << ob.get_fills().size() << " fills)" << std::endl;
}
