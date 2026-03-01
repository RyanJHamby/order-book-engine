// latency_test.cpp
// Benchmarks the full pipeline: pool allocation -> queue ingestion -> matching.
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

    // ---------- Benchmark 1: Pool alloc + matching (per-order latency) ----------
    {
        std::cout << "=== Pool alloc + add_order latency (1M orders) ===\n";
        OrderBook ob;
        ThreadLocalPool<Order> pool(NUM_ORDERS);

        // Warmup: exercise the matching engine code paths.
        {
            OrderBook warmup_ob;
            ThreadLocalPool<Order> warmup_pool(10000);
            for (int i = 0; i < 10000; ++i) {
                Order* o = warmup_pool.allocate();
                *o = {static_cast<std::uint64_t>(i),
                      i % 2 ? OrderType::BUY : OrderType::SELL,
                      price_dist(rng), static_cast<std::uint32_t>(qty_dist(rng))};
                warmup_ob.add_order(*o);
            }
        }
        rng.seed(42);

        std::vector<double> latencies;
        latencies.reserve(NUM_ORDERS);

        for (int i = 0; i < NUM_ORDERS; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();

            Order* o = pool.allocate();
            *o = {static_cast<std::uint64_t>(i),
                  i % 2 ? OrderType::BUY : OrderType::SELL,
                  price_dist(rng), static_cast<std::uint32_t>(qty_dist(rng))};
            ob.add_order(*o);

            auto t1 = std::chrono::high_resolution_clock::now();
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        std::cout << "  Orders: " << NUM_ORDERS
                  << "  Fills: " << ob.fill_count()
                  << "  Slabs: " << pool.slab_count() << "\n";
        print_percentiles(latencies);
    }

    // ---------- Benchmark 2: Full pipeline (pool -> queue -> match) ----------
    {
        std::cout << "\n=== Full pipeline: pool -> queue -> match (1M orders) ===\n";
        LockFreeQueue<Order, Q_SIZE> queue;
        OrderBook ob;
        std::atomic<bool> producer_done{false};

        // Producer: allocate from thread-local pool, push to queue.
        std::thread producer([&]() {
            auto& pool = get_thread_local_pool<Order>(NUM_ORDERS);
            std::mt19937 prng(42);
            std::uniform_real_distribution<double> pd(99.0, 101.0);
            std::uniform_int_distribution<int> qd(1, 100);

            for (int i = 0; i < NUM_ORDERS; ++i) {
                Order* o = pool.allocate();
                *o = {static_cast<std::uint64_t>(i),
                      i % 2 ? OrderType::BUY : OrderType::SELL,
                      pd(prng), static_cast<std::uint32_t>(qd(prng))};
                while (!queue.push(*o)) std::this_thread::yield();
            }
            producer_done.store(true, std::memory_order_release);
        });

        // Consumer / matching thread.
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
                  << "  Fills: " << ob.fill_count() << "\n";
        std::cout << "  Total:  " << total_us / 1000.0 << " ms\n";
        std::cout << "  Avg:    " << per_order_us << " us/order\n";
        std::cout << "  Throughput: "
                  << static_cast<int>(consumed / (total_us / 1'000'000.0))
                  << " orders/sec\n";
    }

    return 0;
}
