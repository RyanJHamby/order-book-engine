// latency_test.cpp
// Benchmarks the full pipeline: queue ingestion + matching engine.
// Measures per-operation latency with P50/P95/P99/P99.9 percentiles.
#include "orderbook.hpp"
#include "order_queue.hpp"
#include "memory_pool.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <cmath>

static void print_percentiles(std::vector<double>& latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    auto N = latencies_ns.size();
    if (N == 0) return;

    auto pct = [&](double p) -> double {
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * (N - 1));
        return latencies_ns[idx];
    };

    double sum = 0;
    for (auto v : latencies_ns) sum += v;
    double mean = sum / N;

    std::cout << "  Mean:   " << mean / 1000.0 << " us\n";
    std::cout << "  P50:    " << pct(50) / 1000.0 << " us\n";
    std::cout << "  P95:    " << pct(95) / 1000.0 << " us\n";
    std::cout << "  P99:    " << pct(99) / 1000.0 << " us\n";
    std::cout << "  P99.9:  " << pct(99.9) / 1000.0 << " us\n";
    std::cout << "  P99.99: " << pct(99.99) / 1000.0 << " us\n";
    std::cout << "  Min:    " << latencies_ns.front() / 1000.0 << " us\n";
    std::cout << "  Max:    " << latencies_ns.back() / 1000.0 << " us\n";
}

int main() {
    constexpr int NUM_ORDERS = 1'000'000;
    constexpr std::size_t Q_SIZE = 65536;

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(99.0, 101.0);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    // Pre-generate orders.
    std::vector<Order> orders;
    orders.reserve(NUM_ORDERS);
    for (int i = 0; i < NUM_ORDERS; ++i) {
        orders.push_back({static_cast<std::uint64_t>(i),
                         i % 2 ? OrderType::BUY : OrderType::SELL,
                         price_dist(rng),
                         static_cast<std::uint32_t>(qty_dist(rng))});
    }

    // ---------- Benchmark 1: Direct matching (hot path) ----------
    {
        std::cout << "=== Direct add_order latency (1M orders) ===\n";
        OrderBook ob;

        // Warmup.
        for (int i = 0; i < 10000; ++i) ob.add_order(orders[i]);
        ob = OrderBook();

        std::vector<double> latencies;
        latencies.reserve(NUM_ORDERS);

        for (int i = 0; i < NUM_ORDERS; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            ob.add_order(orders[i]);
            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        std::cout << "  Orders: " << NUM_ORDERS
                  << "  Fills: " << ob.get_fills().size() << "\n";
        print_percentiles(latencies);
    }

    // ---------- Benchmark 2: Queue -> matching pipeline ----------
    {
        std::cout << "\n=== Queue pipeline latency (1M orders) ===\n";
        LockFreeQueue<Order, Q_SIZE> queue;
        OrderBook ob;
        std::atomic<bool> producer_done{false};

        std::thread producer([&]() {
            for (int i = 0; i < NUM_ORDERS; ++i) {
                while (!queue.push(orders[i])) std::this_thread::yield();
            }
            producer_done.store(true, std::memory_order_release);
        });

        auto t0 = std::chrono::high_resolution_clock::now();
        int consumed = 0;
        Order incoming{0, OrderType::BUY, 0.0, 0};
        while (true) {
            if (queue.pop(incoming)) {
                ob.add_order(incoming);
                consumed++;
            } else if (producer_done.load(std::memory_order_acquire)) {
                while (queue.pop(incoming)) {
                    ob.add_order(incoming);
                    consumed++;
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        producer.join();

        double total_us = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
        double per_order_us = total_us / consumed;

        std::cout << "  Orders: " << consumed
                  << "  Fills: " << ob.get_fills().size() << "\n";
        std::cout << "  Total:  " << total_us / 1000.0 << " ms\n";
        std::cout << "  Avg:    " << per_order_us << " us/order\n";
        std::cout << "  Throughput: "
                  << static_cast<int>(consumed / (total_us / 1'000'000.0))
                  << " orders/sec\n";
    }

    return 0;
}
