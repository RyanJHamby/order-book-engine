#include <gtest/gtest.h>
#include "../include/orderbook.hpp"

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        ob = OrderBook();
    }
    void TearDown() override {}

    OrderBook ob;
};

TEST_F(OrderBookTest, AddBuyOrder) {
    Order buy_order{1, OrderType::BUY, 100.0, 10};
    EXPECT_NO_THROW(ob.add_order(buy_order));
    EXPECT_EQ(ob.bid_count(), 1u);
}

TEST_F(OrderBookTest, AddSellOrder) {
    Order sell_order{1, OrderType::SELL, 200.0, 20};
    EXPECT_NO_THROW(ob.add_order(sell_order));
    EXPECT_EQ(ob.ask_count(), 1u);
}

TEST_F(OrderBookTest, AddMultipleOrders) {
    ob.add_order({1, OrderType::BUY, 100.0, 10});
    ob.add_order({2, OrderType::SELL, 200.0, 20});
    ob.add_order({3, OrderType::BUY, 150.0, 15});

    EXPECT_EQ(ob.bid_count(), 2u);
    EXPECT_EQ(ob.ask_count(), 1u);
}

TEST_F(OrderBookTest, MatchCrossingOrders) {
    ob.add_order({1, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::BUY, 100.0, 10});

    EXPECT_EQ(ob.get_fills().size(), 1u);
    EXPECT_EQ(ob.get_fills()[0].quantity, 10u);
    EXPECT_EQ(ob.bid_count(), 0u);
    EXPECT_EQ(ob.ask_count(), 0u);
}

TEST_F(OrderBookTest, NoMatchWhenPricesDontCross) {
    ob.add_order({1, OrderType::BUY, 100.0, 10});
    ob.add_order({2, OrderType::SELL, 200.0, 20});

    EXPECT_EQ(ob.get_fills().size(), 0u);
    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_EQ(ob.ask_count(), 1u);
}

TEST_F(OrderBookTest, PartialFill) {
    ob.add_order({1, OrderType::SELL, 100.0, 50});
    ob.add_order({2, OrderType::BUY, 100.0, 30});

    EXPECT_EQ(ob.get_fills().size(), 1u);
    EXPECT_EQ(ob.get_fills()[0].quantity, 30u);
    // 20 remaining on the sell side
    EXPECT_EQ(ob.ask_count(), 1u);
    EXPECT_EQ(ob.bid_count(), 0u);
}

TEST_F(OrderBookTest, PriceTimePriority) {
    // Two sells at same price — first one should fill first (FIFO).
    ob.add_order({1, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::SELL, 100.0, 10});
    ob.add_order({3, OrderType::BUY, 100.0, 10});

    EXPECT_EQ(ob.get_fills().size(), 1u);
    EXPECT_EQ(ob.get_fills()[0].sell_order_id, 1u);  // first resting sell
    EXPECT_EQ(ob.ask_count(), 1u);  // second sell still resting
}

TEST_F(OrderBookTest, SweepMultiplePriceLevels) {
    ob.add_order({1, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::SELL, 101.0, 10});
    ob.add_order({3, OrderType::SELL, 102.0, 10});

    // Aggressive buy sweeps through all three levels.
    ob.add_order({4, OrderType::BUY, 102.0, 25});

    EXPECT_EQ(ob.get_fills().size(), 3u);
    // Fills at 100, 101, 102 in that order (lowest ask first).
    EXPECT_DOUBLE_EQ(ob.get_fills()[0].price, 100.0);
    EXPECT_DOUBLE_EQ(ob.get_fills()[1].price, 101.0);
    EXPECT_DOUBLE_EQ(ob.get_fills()[2].price, 102.0);
    // 25 - 10 - 10 = 5 remaining from level 102
    EXPECT_EQ(ob.ask_count(), 1u);
    EXPECT_EQ(ob.bid_count(), 0u);
}

TEST_F(OrderBookTest, CancelOrder) {
    ob.add_order({1, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::SELL, 101.0, 10});

    EXPECT_TRUE(ob.cancel_order(1));
    EXPECT_EQ(ob.ask_count(), 1u);

    // Canceling non-existent order returns false.
    EXPECT_FALSE(ob.cancel_order(999));
}

TEST_F(OrderBookTest, EmptyBookOperations) {
    EXPECT_EQ(ob.bid_count(), 0u);
    EXPECT_EQ(ob.ask_count(), 0u);
    EXPECT_EQ(ob.fill_count(), 0u);
    EXPECT_FALSE(ob.cancel_order(999));
}

TEST_F(OrderBookTest, StressTest) {
    const int num_orders = 1000;

    for (int i = 0; i < num_orders; ++i) {
        OrderType type = (i % 2 == 0) ? OrderType::BUY : OrderType::SELL;
        Order order{static_cast<uint64_t>(i), type, 100.0 + i, static_cast<uint32_t>(i + 1)};
        EXPECT_NO_THROW(ob.add_order(order));
    }

    // Should have produced fills where buy prices >= sell prices.
    EXPECT_GT(ob.get_fills().size(), 0u);
}
