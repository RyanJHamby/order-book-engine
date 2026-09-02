// naive_orderbook.hpp
// Deliberately naive, O(n)-per-operation reference matching engine used
// only as a differential-testing oracle against the optimized OrderBook
// (see test_differential.cpp). Correctness over speed: linear scans, no
// indexes, no price-level map. Never benchmarked, never used in production.
#pragma once

#include "../include/order.hpp"
#include <vector>
#include <map>
#include <cstdint>
#include <algorithm>

class NaiveOrderBook {
public:
    void add_order(Order order) {
        order.timestamp = next_timestamp_++;
        if (order.type == OrderType::BUY) {
            match_buy(order);
        } else {
            match_sell(order);
        }
        if (order.quantity > 0) {
            if (order.type == OrderType::BUY) {
                bids_.push_back(order);
            } else {
                asks_.push_back(order);
            }
        }
    }

    bool cancel_order(std::uint64_t id) {
        for (auto it = bids_.begin(); it != bids_.end(); ++it) {
            if (it->id == id) { bids_.erase(it); return true; }
        }
        for (auto it = asks_.begin(); it != asks_.end(); ++it) {
            if (it->id == id) { asks_.erase(it); return true; }
        }
        return false;
    }

    const std::vector<Fill>& fills() const { return fills_; }

    std::map<double, std::uint32_t, std::greater<double>> bid_levels() const {
        std::map<double, std::uint32_t, std::greater<double>> levels;
        for (const auto& o : bids_) levels[o.price] += o.quantity;
        return levels;
    }

    std::map<double, std::uint32_t> ask_levels() const {
        std::map<double, std::uint32_t> levels;
        for (const auto& o : asks_) levels[o.price] += o.quantity;
        return levels;
    }

private:
    void match_buy(Order& incoming) {
        while (incoming.quantity > 0) {
            auto it = best_ask();
            if (it == asks_.end()) break;
            if (incoming.price < it->price) break;

            std::uint32_t fill_qty = std::min(incoming.quantity, it->quantity);
            fills_.push_back({incoming.id, it->id, it->price, fill_qty});
            incoming.quantity -= fill_qty;
            it->quantity -= fill_qty;
            if (it->quantity == 0) asks_.erase(it);
        }
    }

    void match_sell(Order& incoming) {
        while (incoming.quantity > 0) {
            auto it = best_bid();
            if (it == bids_.end()) break;
            if (incoming.price > it->price) break;

            std::uint32_t fill_qty = std::min(incoming.quantity, it->quantity);
            fills_.push_back({it->id, incoming.id, it->price, fill_qty});
            incoming.quantity -= fill_qty;
            it->quantity -= fill_qty;
            if (it->quantity == 0) bids_.erase(it);
        }
    }

    // Lowest price first, ties broken by earliest timestamp (insertion order)
    // -- same price-time priority the optimized OrderBook implements via its
    // per-level deque FIFO.
    std::vector<Order>::iterator best_ask() {
        auto best = asks_.end();
        for (auto it = asks_.begin(); it != asks_.end(); ++it) {
            if (best == asks_.end() || it->price < best->price ||
                (it->price == best->price && it->timestamp < best->timestamp)) {
                best = it;
            }
        }
        return best;
    }

    // Highest price first, ties broken by earliest timestamp.
    std::vector<Order>::iterator best_bid() {
        auto best = bids_.end();
        for (auto it = bids_.begin(); it != bids_.end(); ++it) {
            if (best == bids_.end() || it->price > best->price ||
                (it->price == best->price && it->timestamp < best->timestamp)) {
                best = it;
            }
        }
        return best;
    }

    std::vector<Order> bids_;
    std::vector<Order> asks_;
    std::vector<Fill> fills_;
    std::uint64_t next_timestamp_{0};
};
