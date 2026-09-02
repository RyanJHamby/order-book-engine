// test_differential.cpp
// Differential/property-based testing: cross-check the optimized OrderBook
// against a deliberately naive, obviously-correct O(n) reference
// implementation (NaiveOrderBook) on randomized operation sequences.
//
// Hand-written unit tests only prove the engine handles the cases someone
// thought to write down. This proves something stronger: two independent
// implementations of the same price-time-priority spec agree on every
// randomized add/cancel sequence across many seeds -- fills, resting book
// state, and cancel results, checked after every single operation, not
// just at the end of a run.
#include <gtest/gtest.h>
#include "../include/orderbook.hpp"
#include "naive_orderbook.hpp"
#include <random>
#include <vector>

namespace {

enum class OpKind { ADD, CANCEL };

struct Op {
    OpKind kind;
    Order order;             // valid when kind == ADD
    std::uint64_t cancel_id; // valid when kind == CANCEL
};

// A discrete price ladder (not continuous std::uniform_real_distribution)
// is essential here, not cosmetic: with continuous doubles, two orders
// essentially never land on the exact same price, so same-price FIFO
// tie-breaking -- a real, previously-broken-and-silently-passing case
// when this test used continuous prices -- would never actually get
// exercised. 10 discrete ticks over 300 ops guarantees frequent same-price
// collisions. ~20% of ops are cancels, including cancels of ids that have
// already been filled or cancelled -- a real, worth-testing no-op case on
// both engines, not a bug in the generator.
std::vector<Op> generate_sequence(std::mt19937& rng, int num_ops) {
    std::uniform_int_distribution<int> tick_dist(0, 9);
    std::uniform_int_distribution<int> qty_dist(1, 50);
    std::uniform_int_distribution<int> choice_dist(0, 9);

    std::vector<Op> ops;
    std::vector<std::uint64_t> seen_ids;
    std::uint64_t next_id = 1;

    for (int i = 0; i < num_ops; ++i) {
        bool do_cancel = choice_dist(rng) >= 8 && !seen_ids.empty();
        if (do_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, seen_ids.size() - 1);
            ops.push_back({OpKind::CANCEL, Order{}, seen_ids[pick(rng)]});
        } else {
            OrderType type = (choice_dist(rng) % 2 == 0) ? OrderType::BUY : OrderType::SELL;
            double price = 95.0 + tick_dist(rng);
            Order o{next_id, type, price, static_cast<std::uint32_t>(qty_dist(rng))};
            ops.push_back({OpKind::ADD, o, 0});
            seen_ids.push_back(next_id);
            ++next_id;
        }
    }
    return ops;
}

void assert_fills_match(const OrderBook& ob, const NaiveOrderBook& naive,
                         int seed, int step) {
    const auto& a = ob.get_fills();
    const auto& b = naive.fills();
    ASSERT_EQ(a.size(), b.size())
        << "seed=" << seed << " step=" << step << ": fill count diverged";
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i].buy_order_id, b[i].buy_order_id)
            << "seed=" << seed << " step=" << step << " fill=" << i;
        ASSERT_EQ(a[i].sell_order_id, b[i].sell_order_id)
            << "seed=" << seed << " step=" << step << " fill=" << i;
        ASSERT_DOUBLE_EQ(a[i].price, b[i].price)
            << "seed=" << seed << " step=" << step << " fill=" << i;
        ASSERT_EQ(a[i].quantity, b[i].quantity)
            << "seed=" << seed << " step=" << step << " fill=" << i;
    }
}

template <typename MapA, typename MapB>
void assert_levels_match(const MapA& a, const MapB& b, const char* side,
                          int seed, int step) {
    ASSERT_EQ(a.size(), b.size())
        << "seed=" << seed << " step=" << step << ": " << side << " level count diverged";
    auto ait = a.begin();
    auto bit = b.begin();
    for (; ait != a.end(); ++ait, ++bit) {
        ASSERT_DOUBLE_EQ(ait->first, bit->first)
            << "seed=" << seed << " step=" << step << " side=" << side;
        ASSERT_EQ(ait->second, bit->second)
            << "seed=" << seed << " step=" << step << " side=" << side
            << " price=" << ait->first;
    }
}

void assert_books_match(const OrderBook& ob, const NaiveOrderBook& naive,
                         int seed, int step) {
    assert_fills_match(ob, naive, seed, step);
    assert_levels_match(ob.bid_book_snapshot(), naive.bid_levels(), "bid", seed, step);
    assert_levels_match(ob.ask_book_snapshot(), naive.ask_levels(), "ask", seed, step);
}

} // namespace

TEST(DifferentialTest, RandomizedOpSequencesMatchReference) {
    constexpr int kNumSeeds = 200;
    constexpr int kOpsPerSeed = 300;

    for (int seed = 0; seed < kNumSeeds; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        auto ops = generate_sequence(rng, kOpsPerSeed);

        OrderBook ob;
        NaiveOrderBook naive;

        for (int step = 0; step < kOpsPerSeed; ++step) {
            const Op& op = ops[step];
            if (op.kind == OpKind::CANCEL) {
                bool ob_result = ob.cancel_order(op.cancel_id);
                bool naive_result = naive.cancel_order(op.cancel_id);
                ASSERT_EQ(ob_result, naive_result)
                    << "seed=" << seed << " step=" << step
                    << ": cancel result diverged for id=" << op.cancel_id;
            } else {
                ob.add_order(op.order);
                naive.add_order(op.order);
            }
            // ASSERT_* inside a helper only aborts that helper, not this
            // loop -- wrap it so a real divergence stops the sequence
            // immediately instead of cascading into hundreds of downstream
            // failures once the two books have already diverged.
            assert_books_match(ob, naive, seed, step);
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
}
