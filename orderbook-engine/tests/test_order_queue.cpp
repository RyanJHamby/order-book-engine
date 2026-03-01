#include <gtest/gtest.h>
#include "../include/order_queue.hpp"
#include "../include/order.hpp"
#include <thread>
#include <vector>

class OrderQueueTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(OrderQueueTest, BasicPushPop) {
    LockFreeQueue<Order, 10> queue;
    
    Order order{1, OrderType::BUY, 100.0, 10};
    
    // Push should succeed
    EXPECT_TRUE(queue.push(order));
    
    Order popped_order{0, OrderType::SELL, 0.0, 0};
    EXPECT_TRUE(queue.pop(popped_order));
    
    EXPECT_EQ(popped_order.id, order.id);
    EXPECT_EQ(popped_order.type, order.type);
    EXPECT_DOUBLE_EQ(popped_order.price, order.price);
    EXPECT_EQ(popped_order.quantity, order.quantity);
}

TEST_F(OrderQueueTest, EmptyQueuePop) {
    LockFreeQueue<Order, 10> queue;
    
    Order order{0, OrderType::BUY, 0.0, 0};
    EXPECT_FALSE(queue.pop(order));
}

TEST_F(OrderQueueTest, FullQueuePush) {
    // Ring buffer of size N holds N-1 items (one sentinel slot).
    LockFreeQueue<Order, 4> queue;

    Order order1{1, OrderType::BUY, 100.0, 10};
    Order order2{2, OrderType::SELL, 200.0, 20};
    Order order3{3, OrderType::BUY, 300.0, 30};
    Order order4{4, OrderType::SELL, 400.0, 40};

    EXPECT_TRUE(queue.push(order1));
    EXPECT_TRUE(queue.push(order2));
    EXPECT_TRUE(queue.push(order3));
    EXPECT_FALSE(queue.push(order4)); // Should fail - queue is full
}

TEST_F(OrderQueueTest, CircularBehavior) {
    LockFreeQueue<Order, 4> queue;

    Order order1{1, OrderType::BUY, 100.0, 10};
    Order order2{2, OrderType::SELL, 200.0, 20};
    Order order3{3, OrderType::BUY, 300.0, 30};

    // Fill to capacity (N-1 = 3 items).
    EXPECT_TRUE(queue.push(order1));
    EXPECT_TRUE(queue.push(order2));
    EXPECT_TRUE(queue.push(order3));

    // Pop one to free a slot.
    Order popped{0, OrderType::SELL, 0.0, 0};
    EXPECT_TRUE(queue.pop(popped));
    EXPECT_EQ(popped.id, 1u);

    // Should wrap around and push again.
    Order order4{4, OrderType::SELL, 400.0, 40};
    EXPECT_TRUE(queue.push(order4));
}

TEST_F(OrderQueueTest, MultipleOrders) {
    LockFreeQueue<Order, 100> queue;
    const int num_orders = 50;
    
    // Push multiple orders
    for (int i = 0; i < num_orders; ++i) {
        OrderType type = (i % 2 == 0) ? OrderType::BUY : OrderType::SELL;
        Order order{static_cast<uint64_t>(i), type, 100.0 + i, static_cast<uint32_t>(i + 1)};
        EXPECT_TRUE(queue.push(order));
    }
    
    // Pop all orders and verify
    for (int i = 0; i < num_orders; ++i) {
        Order popped{0, OrderType::SELL, 0.0, 0};
        EXPECT_TRUE(queue.pop(popped));
        EXPECT_EQ(popped.id, i);
    }
    
    // Queue should be empty now
    Order empty{0, OrderType::SELL, 0.0, 0};
    EXPECT_FALSE(queue.pop(empty));
}
