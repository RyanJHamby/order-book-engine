// Spec reject-duplicate-order-id: an order whose id is already resting on the
// book is ignored. Registered as its own CTest target with WILL_FAIL until the
// spec is implemented (see tests/CMakeLists.txt).
#include <gtest/gtest.h>
#include "../include/orderbook.hpp"

class DuplicateOrderIdTest : public ::testing::Test {
protected:
    OrderBook ob;
};

TEST_F(DuplicateOrderIdTest, SecondOrderWithRestingIdIsIgnored) {
    ob.add_order({1, OrderType::BUY, 100.0, 10});
    ob.add_order({1, OrderType::BUY, 101.0, 5});  // same id, different price

    EXPECT_EQ(ob.bid_count(), 1u);
    auto bids = ob.bid_book_snapshot();
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.begin()->first, 100.0);
    EXPECT_EQ(bids.begin()->second, 10u);
}

TEST_F(DuplicateOrderIdTest, OriginalOrderRemainsCancellableExactlyOnce) {
    ob.add_order({1, OrderType::BUY, 100.0, 10});
    ob.add_order({1, OrderType::BUY, 101.0, 5});

    EXPECT_TRUE(ob.cancel_order(1));
    EXPECT_EQ(ob.bid_count(), 0u);  // no ghost order left behind
    EXPECT_FALSE(ob.cancel_order(1));
}

TEST_F(DuplicateOrderIdTest, DuplicateThatWouldCrossProducesNoFill) {
    ob.add_order({2, OrderType::SELL, 100.0, 10});
    ob.add_order({2, OrderType::BUY, 100.0, 10});  // would fill, but id 2 is resting

    EXPECT_EQ(ob.fill_count(), 0u);
    EXPECT_EQ(ob.ask_count(), 1u);
    EXPECT_EQ(ob.bid_count(), 0u);
}

TEST_F(DuplicateOrderIdTest, DuplicateOnTheOppositeSideIsAlsoIgnored) {
    ob.add_order({3, OrderType::BUY, 100.0, 10});
    ob.add_order({3, OrderType::SELL, 50.0, 4});

    EXPECT_EQ(ob.fill_count(), 0u);
    EXPECT_EQ(ob.ask_count(), 0u);
    EXPECT_EQ(ob.bid_book_snapshot().at(100.0), 10u);
}

TEST_F(DuplicateOrderIdTest, PartiallyFilledRestingIdStillCountsAsResting) {
    ob.add_order({4, OrderType::SELL, 100.0, 10});
    ob.add_order({5, OrderType::BUY, 100.0, 4});   // fills 4 of order 4
    ob.add_order({4, OrderType::SELL, 90.0, 7});   // id 4 still resting with 6

    EXPECT_EQ(ob.fill_count(), 1u);
    auto asks = ob.ask_book_snapshot();
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks.at(100.0), 6u);
}

TEST_F(DuplicateOrderIdTest, IdCanBeReusedAfterFullFill) {
    ob.add_order({6, OrderType::SELL, 100.0, 10});
    ob.add_order({7, OrderType::BUY, 100.0, 10});  // fully fills order 6
    ob.add_order({6, OrderType::BUY, 99.0, 5});    // id 6 is no longer resting

    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_TRUE(ob.cancel_order(6));
}

TEST_F(DuplicateOrderIdTest, IdCanBeReusedAfterCancel) {
    ob.add_order({8, OrderType::BUY, 100.0, 10});
    ASSERT_TRUE(ob.cancel_order(8));
    ob.add_order({8, OrderType::BUY, 98.0, 3});

    EXPECT_EQ(ob.bid_count(), 1u);
    EXPECT_EQ(ob.bid_book_snapshot().at(98.0), 3u);
}

TEST_F(DuplicateOrderIdTest, IncomingOrderThatFullyFillsNeverBecomesRestingSoItsIdIsFree) {
    ob.add_order({9, OrderType::SELL, 100.0, 10});
    ob.add_order({10, OrderType::BUY, 100.0, 10});  // id 10 filled on arrival, never rests
    ob.add_order({10, OrderType::BUY, 95.0, 2});    // legitimate new order

    EXPECT_EQ(ob.bid_count(), 1u);
}
