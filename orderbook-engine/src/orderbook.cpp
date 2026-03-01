// orderbook.cpp
#include "orderbook.hpp"
#include <algorithm>

void OrderBook::add_order(const Order& order) {
    Order incoming = order;
    incoming.timestamp = next_timestamp_++;

    if (incoming.type == OrderType::BUY) {
        try_match_buy(incoming);
    } else {
        try_match_sell(incoming);
    }

    // Rest any unfilled quantity on the book.
    if (incoming.quantity > 0) {
        order_index_[incoming.id] = {incoming.price, incoming.type};
        if (incoming.type == OrderType::BUY) {
            bids_[incoming.price].push_back(incoming);
        } else {
            asks_[incoming.price].push_back(incoming);
        }
    }
}

inline void OrderBook::try_match_buy(Order& incoming) {
    // Match incoming buy against resting asks (lowest price first).
    while (incoming.quantity > 0 && !asks_.empty()) {
        auto best_ask_it = asks_.begin();
        if (incoming.price < best_ask_it->first) break;

        auto& ask_queue = best_ask_it->second;
        while (incoming.quantity > 0 && !ask_queue.empty()) {
            Order& resting = ask_queue.front();
            std::uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

            fills_.push_back({incoming.id, resting.id, resting.price, fill_qty});

            incoming.quantity -= fill_qty;
            resting.quantity -= fill_qty;

            if (resting.quantity == 0) {
                order_index_.erase(resting.id);
                ask_queue.pop_front();
            }
        }

        if (ask_queue.empty()) {
            asks_.erase(best_ask_it);
        }
    }
}

inline void OrderBook::try_match_sell(Order& incoming) {
    // Match incoming sell against resting bids (highest price first).
    while (incoming.quantity > 0 && !bids_.empty()) {
        auto best_bid_it = bids_.begin();
        if (incoming.price > best_bid_it->first) break;

        auto& bid_queue = best_bid_it->second;
        while (incoming.quantity > 0 && !bid_queue.empty()) {
            Order& resting = bid_queue.front();
            std::uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

            fills_.push_back({resting.id, incoming.id, resting.price, fill_qty});

            incoming.quantity -= fill_qty;
            resting.quantity -= fill_qty;

            if (resting.quantity == 0) {
                order_index_.erase(resting.id);
                bid_queue.pop_front();
            }
        }

        if (bid_queue.empty()) {
            bids_.erase(best_bid_it);
        }
    }
}

void OrderBook::match_orders() {
    // Continuous matching resolves all crosses in add_order().
    // This method exists for API compatibility.
}

bool OrderBook::cancel_order(std::uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    auto [price, side] = it->second;
    order_index_.erase(it);

    if (side == OrderType::BUY) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            auto& q = level_it->second;
            q.erase(std::remove_if(q.begin(), q.end(),
                [order_id](const Order& o) { return o.id == order_id; }), q.end());
            if (q.empty()) bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            auto& q = level_it->second;
            q.erase(std::remove_if(q.begin(), q.end(),
                [order_id](const Order& o) { return o.id == order_id; }), q.end());
            if (q.empty()) asks_.erase(level_it);
        }
    }

    return true;
}

std::size_t OrderBook::bid_count() const {
    std::size_t count = 0;
    for (const auto& [price, queue] : bids_) count += queue.size();
    return count;
}

std::size_t OrderBook::ask_count() const {
    std::size_t count = 0;
    for (const auto& [price, queue] : asks_) count += queue.size();
    return count;
}
