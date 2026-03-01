#include <gtest/gtest.h>
#include "../include/orderbook.hpp"
#include "../include/order_queue.hpp"
#include "../include/memory_pool.hpp"
#include <limits>

class EdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        ob = OrderBook();
    }
    void TearDown() override {}

    OrderBook ob;
};

TEST_F(EdgeCaseTest, OrderExtremeValues) {
    Order max_order{
        std::numeric_limits<uint64_t>::max(),
        OrderType::BUY,
        std::numeric_limits<double>::max(),
        std::numeric_limits<uint32_t>::max()
    };

    EXPECT_NO_THROW(ob.add_order(max_order));
    EXPECT_EQ(ob.bid_count(), 1u);
}

TEST_F(EdgeCaseTest, OrderZeroQuantity) {
    Order zero_order{0, OrderType::SELL, 100.0, 0};
    ob.add_order(zero_order);
    // Zero-quantity order should not rest on the book.
    EXPECT_EQ(ob.ask_count(), 0u);
}

TEST_F(EdgeCaseTest, OrderNegativePrice) {
    Order negative{1, OrderType::BUY, -100.0, 10};
    EXPECT_NO_THROW(ob.add_order(negative));
    EXPECT_EQ(ob.bid_count(), 1u);
}

TEST_F(EdgeCaseTest, OrderVerySmallPrice) {
    Order small_price{1, OrderType::SELL, 0.000001, 1};
    EXPECT_NO_THROW(ob.add_order(small_price));
    EXPECT_EQ(ob.ask_count(), 1u);
}

TEST_F(EdgeCaseTest, OrderBookEmptyOperations) {
    EXPECT_NO_THROW(ob.match_orders());
    EXPECT_FALSE(ob.cancel_order(999));

    Order single{1, OrderType::BUY, 100.0, 10};
    ob.add_order(single);
    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_NO_THROW(ob.match_orders());
}

TEST_F(EdgeCaseTest, OrderBookSingleOrderType) {
    // Only buys — nothing to match.
    for (int i = 0; i < 10; ++i) {
        ob.add_order({static_cast<uint64_t>(i), OrderType::BUY, 100.0 + i, 10});
    }
    EXPECT_EQ(ob.bid_count(), 10u);
    EXPECT_EQ(ob.get_fills().size(), 0u);

    // Only sells.
    OrderBook ob2;
    for (int i = 0; i < 10; ++i) {
        ob2.add_order({static_cast<uint64_t>(i), OrderType::SELL, 200.0 + i, 10});
    }
    EXPECT_EQ(ob2.ask_count(), 10u);
    EXPECT_EQ(ob2.get_fills().size(), 0u);
}

TEST_F(EdgeCaseTest, OrderBookIdenticalOrders) {
    // Identical buy orders — all rest, no sells to match.
    for (int i = 0; i < 5; ++i) {
        ob.add_order({static_cast<uint64_t>(100 + i), OrderType::BUY, 100.0, 10});
    }
    EXPECT_EQ(ob.bid_count(), 5u);
}

TEST_F(EdgeCaseTest, LockFreeQueueEdgeCases) {
    LockFreeQueue<Order, 2> tiny_queue;

    Order order1{1, OrderType::BUY, 100.0, 10};
    Order order2{2, OrderType::SELL, 200.0, 20};

    EXPECT_TRUE(tiny_queue.push(order1));
    EXPECT_FALSE(tiny_queue.push(order2));

    Order popped{0, OrderType::SELL, 0.0, 0};
    EXPECT_TRUE(tiny_queue.pop(popped));
    EXPECT_EQ(popped.id, 1u);
    EXPECT_FALSE(tiny_queue.pop(popped));
}

TEST_F(EdgeCaseTest, LockFreeQueueCircularWrap) {
    LockFreeQueue<Order, 4> small_queue;

    EXPECT_TRUE(small_queue.push({1, OrderType::BUY, 100.0, 10}));
    EXPECT_TRUE(small_queue.push({2, OrderType::SELL, 200.0, 20}));
    EXPECT_TRUE(small_queue.push({3, OrderType::BUY, 300.0, 30}));

    Order popped{0, OrderType::SELL, 0.0, 0};
    EXPECT_TRUE(small_queue.pop(popped));
    EXPECT_EQ(popped.id, 1u);

    // Wraps around after pop.
    EXPECT_TRUE(small_queue.push({4, OrderType::SELL, 400.0, 40}));
}

TEST_F(EdgeCaseTest, MemoryPoolEdgeCases) {
    ThreadLocalPool<Order> pool(0);

    Order* order = pool.allocate();
    EXPECT_NE(order, nullptr);

    ThreadLocalPool<Order> large_pool(1000000);
    std::vector<Order*> allocations;
    for (int i = 0; i < 1000; ++i) {
        Order* o = large_pool.allocate();
        EXPECT_NE(o, nullptr);
        allocations.push_back(o);
    }

    for (std::size_t i = 0; i < allocations.size(); ++i) {
        for (std::size_t j = i + 1; j < allocations.size(); ++j) {
            EXPECT_NE(allocations[i], allocations[j]);
        }
    }
}

TEST_F(EdgeCaseTest, CancelNonExistentOrder) {
    ob.add_order({1, OrderType::BUY, 100.0, 10});
    EXPECT_FALSE(ob.cancel_order(999));
    EXPECT_EQ(ob.bid_count(), 1u);
}

TEST_F(EdgeCaseTest, CancelAlreadyFilledOrder) {
    ob.add_order({1, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::BUY, 100.0, 10});  // fills order 1 completely
    EXPECT_FALSE(ob.cancel_order(1));  // already gone
}

TEST_F(EdgeCaseTest, OrderBookStressTest) {
    const int num_orders = 100000;

    for (int i = 0; i < num_orders; ++i) {
        ob.add_order({static_cast<uint64_t>(i),
                     (i % 2 == 0) ? OrderType::BUY : OrderType::SELL,
                     100.0 + (i % 1000),
                     static_cast<uint32_t>((i % 100) + 1)});
    }

    EXPECT_GT(ob.get_fills().size(), 0u);
}
