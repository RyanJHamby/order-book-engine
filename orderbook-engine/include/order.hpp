// order.hpp
#pragma once
#include <cstdint>

enum class OrderType { BUY, SELL };

struct Order {
    std::uint64_t id;
    OrderType type;
    double price;
    std::uint32_t quantity;
    std::uint64_t timestamp{0};
};

struct Fill {
    std::uint64_t buy_order_id;
    std::uint64_t sell_order_id;
    double price;
    std::uint32_t quantity;
};
