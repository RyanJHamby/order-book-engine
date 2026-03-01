// orderbook.hpp
#pragma once

#include "order.hpp"
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <cstddef>

class OrderBook {
public:
    // Add order and immediately match against resting orders (continuous matching).
    void add_order(const Order& order);

    // Sweep for any remaining crosses. With continuous matching this is a no-op
    // since all crosses are resolved inline in add_order().
    void match_orders();

    // Cancel a resting order by ID. Returns true if found and removed.
    bool cancel_order(std::uint64_t order_id);

    const std::vector<Fill>& get_fills() const { return fills_; }
    std::size_t bid_count() const;
    std::size_t ask_count() const;
    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

private:
    // Price levels: bids highest-first, asks lowest-first (price-time priority).
    std::map<double, std::deque<Order>, std::greater<double>> bids_;
    std::map<double, std::deque<Order>> asks_;

    // O(1) cancel lookup: order_id -> {price, side}
    std::unordered_map<std::uint64_t, std::pair<double, OrderType>> order_index_;

    std::vector<Fill> fills_;
    std::uint64_t next_timestamp_{0};

    inline void try_match_buy(Order& incoming);
    inline void try_match_sell(Order& incoming);
};
