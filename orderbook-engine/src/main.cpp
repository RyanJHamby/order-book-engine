// main.cpp
#include "orderbook.hpp"
#include <iostream>

int main() {
    OrderBook book;

    // Resting asks on the book.
    book.add_order({1, OrderType::SELL, 101.0, 50});
    book.add_order({2, OrderType::SELL, 102.0, 30});
    book.add_order({3, OrderType::SELL, 100.5, 20});

    // Aggressive buy sweeps through price levels.
    book.add_order({4, OrderType::BUY, 102.0, 60});

    std::cout << "Fills:\n";
    for (const auto& f : book.get_fills()) {
        std::cout << "  buy=" << f.buy_order_id
                  << " sell=" << f.sell_order_id
                  << " @ " << f.price
                  << " qty=" << f.quantity << "\n";
    }

    std::cout << "Resting bids: " << book.bid_count()
              << "  Resting asks: " << book.ask_count() << "\n";

    // Cancel a resting order.
    book.cancel_order(2);
    std::cout << "After cancel(2) asks: " << book.ask_count() << "\n";

    return 0;
}
