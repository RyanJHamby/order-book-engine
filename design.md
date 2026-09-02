# Design Document: Low-Latency Order Book Engine

## Overview

A high-performance C++20 order matching engine built for sub-microsecond latency. Implements continuous price-time priority matching with lock-free order ingestion, thread-local memory pools, and automated EC2 Spot benchmarking with P99.9 tail latency profiling.

### Performance

Benchmarked with 1,000,000 orders (alternating buy/sell, random prices, real matching with 773K fills). Pool allocation + matching in the timed loop.

| Metric | Apple Silicon (local, informal) | EC2 `c6i.large` (isolated core, 3 runs) |
|--------|---|---|
| P50 latency | 0.21 μs | 0.208–0.214 μs |
| P95 latency | 1.5 μs | 1.26–1.32 μs |
| P99 latency | 2.6 μs | 1.95–2.07 μs |
| P99.9 latency | 4.7–5.2 μs | 3.09–3.23 μs |
| Pipeline throughput | 2.5–2.7M orders/sec | 1.42–1.80M orders/sec |

EC2 numbers are the ones to cite — isolated core (`taskset`), no turbo, `performance` governor. See the main README's Performance section for why EC2 throughput trails Apple Silicon despite better latency percentiles (2 vCPUs, real `std::map` rebalancing cost visible in the flame graph under sustained load).

---

## Architecture

```
Producer Thread                    Matching Thread
+-----------------+    SPSC     +--------------------+
| ThreadLocalPool | -> Queue -> | OrderBook          |
| (slab alloc)    |            | (price-time match) |
+-----------------+            +--------------------+
                                        |
                                   Fill Reports
```

Three core components form a pipeline:

1. **ThreadLocalPool\<T\>** — Per-thread slab allocator eliminating malloc from the hot path
2. **LockFreeQueue\<T, N\>** — SPSC lock-free ring buffer for cross-thread order ingestion
3. **OrderBook** — Single-threaded continuous matching engine with price-time priority

The design mirrors production exchange architecture (CME Globex, NASDAQ ITCH): N producers feed a single matching thread via lock-free queues, keeping the matching engine single-threaded to avoid synchronization overhead entirely.

---

## Component Design

### 1. Memory Pool (`include/memory_pool.hpp`)

**Problem:** `std::malloc` in the hot path introduces syscall overhead, heap fragmentation, and non-deterministic latency (jitter). A single malloc can cost 100+ ns — unacceptable when the target is sub-microsecond total latency.

**Solution:** Thread-local slab-based bump-pointer allocator.

```
Slab 0: [Order 0][Order 1]...[Order N-1]
Slab 1: [Order N][Order N+1]...[Order 2N-1]
Slab 2: ...
```

**Allocation path:**
```cpp
T* allocate() {
    if (index_ >= slab_capacity_) {
        allocate_slab();  // Rare: only when current slab exhausted
    }
    return &current_slab_[index_++];  // Hot path: single increment
}
```

**Key design decisions:**

| Decision | Rationale |
|----------|-----------|
| `thread_local` storage | Zero contention — no locks, no atomics in allocation path |
| Slab-based (not vector) | Pointers remain stable across growth. Vector reallocation would invalidate all outstanding pointers |
| Bump-pointer allocation | O(1) — single index increment, no free-list traversal |
| Default slab capacity 4096 | Balances memory waste vs malloc frequency. 4096 orders × ~32 bytes = ~128KB per slab, fits in L2 cache |
| No deallocation | Orders are allocated and consumed; pool resets only on destruction. Avoids free-list complexity in the hot path |

**Cache behavior:** Sequential allocation within a slab means consecutive orders are adjacent in memory, improving spatial locality when the matching engine iterates over them.

---

### 2. Lock-Free Queue (`include/order_queue.hpp`)

**Problem:** A mutex-protected queue introduces lock contention, syscalls, and context switches between producer and consumer threads. Even an uncontended mutex acquire/release costs ~25 ns on modern hardware.

**Solution:** SPSC (single-producer, single-consumer) lock-free ring buffer with acquire-release memory ordering.

**Ring buffer layout:**
```
[0][1][2]...[N-2][N-1]
 ^                 ^
 head (consumer)   tail (producer)
```

- Fixed capacity `N` with usable slots = `N-1` (one sentinel for full/empty disambiguation)
- `head_` tracks consumer read position, `tail_` tracks producer write position
- Both wrap modulo `N` for circular behavior

**Memory ordering strategy:**

```cpp
bool push(const T& item) {
    size_t t = tail_.load(memory_order_relaxed);      // Read own position (no fence needed)
    size_t next = (t + 1) % N;
    if (next == head_.load(memory_order_acquire))      // Acquire consumer's head
        return false;                                   // Queue full
    buffer_[t] = item;
    tail_.store(next, memory_order_release);           // Release: make data visible to consumer
    return true;
}

bool pop(T& item) {
    size_t h = head_.load(memory_order_relaxed);       // Read own position
    if (h == tail_.load(memory_order_acquire))          // Acquire producer's tail
        return false;                                   // Queue empty
    item = buffer_[h];
    head_.store((h + 1) % N, memory_order_release);    // Release: signal space available
    return true;
}
```

**Why acquire-release (not sequential consistency):**
- `memory_order_seq_cst` (the default) inserts full memory barriers on every atomic operation — expensive on x86 and catastrophic on ARM
- Acquire-release is sufficient for SPSC: producer releases writes for consumer to acquire, consumer releases reads for producer to acquire
- This is **wait-free** on both sides — neither producer nor consumer ever spins on the other

**False sharing prevention:**

```cpp
alignas(CACHE_LINE) std::atomic<size_t> head_;  // Own cache line
alignas(CACHE_LINE) std::atomic<size_t> tail_;  // Different cache line
```

Without alignment, `head_` and `tail_` may share a 64-byte cache line. When the producer writes `tail_`, it invalidates the consumer's cached copy of `head_` (and vice versa), causing cross-core cache transfers on every operation. This false sharing can degrade latency by ~100x. Cache-line alignment eliminates it entirely.

---

### 3. Matching Engine (`include/orderbook.hpp`, `src/orderbook.cpp`)

**Problem:** Match incoming orders against resting orders with price-time priority at minimal latency.

**Solution:** Single-threaded continuous matching engine using sorted maps for price levels and deques for time-priority queues within each level.

**Data structures:**

```cpp
std::map<double, std::deque<Order>, std::greater<double>> bids_;  // Best bid = begin()
std::map<double, std::deque<Order>> asks_;                        // Best ask = begin()
std::unordered_map<uint64_t, std::pair<double, OrderType>> order_index_;  // O(1) cancel
std::vector<Fill> fills_;
uint64_t next_timestamp_{0};
```

| Structure | Purpose | Complexity |
|-----------|---------|------------|
| `map<price, deque>` for bids | Sorted descending so `begin()` = best bid | O(log P) insert/find by price level |
| `map<price, deque>` for asks | Sorted ascending so `begin()` = best ask | O(log P) insert/find by price level |
| `deque<Order>` per level | FIFO within price level (time priority) | O(1) front/push_back |
| `unordered_map<id, ...>` | Cancel lookup by order ID | O(1) average |

**Matching algorithm (buy order):**

```
1. While incoming.quantity > 0 AND asks not empty:
   a. best_ask = asks.begin() (lowest resting ask)
   b. If incoming.price < best_ask.price → no cross, stop
   c. For each resting order at best_ask level (FIFO):
      - fill_qty = min(incoming.qty, resting.qty)
      - Record Fill at resting order's price
      - Reduce both quantities
      - Remove resting order if fully filled
   d. Remove price level if empty
2. If incoming.quantity > 0, rest remainder on bids
```

Sell order matching mirrors this against bids (highest first).

**Fill execution rule:** Fills execute at the **resting order's price**, matching standard exchange semantics. An aggressive buy at $102 matching a resting ask at $100 fills at $100.

**Why single-threaded:**
- Eliminates all synchronization overhead (locks, atomics, memory fences) in the matching hot path
- No cache coherency traffic between cores
- Deterministic latency — no contention-dependent jitter
- Mirrors real exchange architecture: CME Globex runs one matching thread per product

The lock-free queue handles the multi-threaded boundary at ingestion. Everything after the queue is single-threaded by design.

**Order cancellation:**

```cpp
bool cancel_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);     // O(1) lookup
    if (it == order_index_.end()) return false;
    auto [price, side] = it->second;
    order_index_.erase(it);
    // Find and remove from the price level's deque
    // O(log P) to find level + O(K) to scan deque at that level
}
```

The `order_index_` unordered map provides O(1) lookup by ID, avoiding a linear scan across all price levels and orders.

---

## Compiler Optimization Strategy

```cmake
-O3 -march=native -flto -fno-exceptions -fno-rtti -pthread -mavx2
```

| Flag | Purpose | Impact |
|------|---------|--------|
| `-O3` | Aggressive optimization: loop unrolling, auto-vectorization, function inlining | 2-3x throughput vs `-O1` |
| `-march=native` | Enable all CPU features available on host (AVX2, BMI2, SSE4.2) | SIMD auto-vectorization |
| `-flto` | Link-time optimization: cross-translation-unit inlining | **Critical** — allows matching hot path to inline across `.cpp` boundaries |
| `-fno-exceptions` | Remove exception handling machinery | Eliminates stack unwinding tables, reduces code size |
| `-fno-rtti` | Disable runtime type information | No vtable overhead, smaller binary |
| `-mavx2` | Explicit AVX2 on x86_64 | 256-bit vector operations |

**Why LTO is critical:** Without LTO, the matching engine loop in `orderbook.cpp` cannot be inlined into the benchmark loop in `latency_test.cpp`. They're separate translation units. LTO enables the compiler to see across `.o` files and flatten the call stack, eliminating function call overhead in the timed hot path.

Two CMake options trade these release flags for instrumentation when needed:
`ENABLE_TSAN` swaps them for `-fsanitize=thread -O1 -g -fno-omit-frame-pointer`
(no LTO — TSan needs accurate stacks); `ENABLE_PROFILING` keeps the release
flags but adds `-g -fno-omit-frame-pointer` so `perf` can symbolize an
otherwise-optimized binary.

---

## Benchmark Methodology

### Benchmark 1: Per-Order Latency (Pool + Matching)

```cpp
// Warmup: 10K orders to populate caches
for (int i = 0; i < 1'000'000; i++) {
    auto t0 = high_resolution_clock::now();
    Order* o = pool.allocate();
    *o = {id, type, random_price, random_qty};
    ob.add_order(*o);
    auto t1 = high_resolution_clock::now();
    latencies.push_back(t1 - t0);
}
// Sort and extract P50/P95/P99/P99.9
```

Measures the full critical path: slab allocation + order initialization + matching (which may trigger 0 or more fills). Alternating buy/sell with random prices in a tight range produces realistic crossing behavior — 773K fills across 1M orders.

### Benchmark 2: Full Pipeline Throughput

```cpp
LockFreeQueue<Order, 65536> queue;
// Producer thread: pool.allocate() → queue.push()
// Consumer thread: queue.pop() → ob.add_order()
// Measure: wall clock / orders consumed = per-order latency
```

Measures end-to-end including cross-thread communication overhead. ~2.5–2.7M orders/sec on Apple Silicon, ~1.42–1.80M orders/sec on isolated EC2 `c6i.large` — see the Performance section above.

### Benchmark 3: Baseline Comparison

```cpp
// Identical producer/consumer pipeline to Benchmark 2 -- same pool, same
// OrderBook, same order generation -- with only the queue swapped for
// std::mutex + std::queue + std::condition_variable.
```

Isolates the one variable that matters: does the lock-free queue actually
beat the obvious alternative? Measured (Apple Silicon, informal): lock-free
SPSC 2.26M orders/sec vs. mutex+queue 2.17M orders/sec — a 1.04x speedup.
Small, and reported as measured rather than reframed: `OrderBook::add_order`
(`std::map` rebalancing, confirmed dominant cost by the EC2 flame graph)
takes roughly the same time regardless of which queue feeds it, so the
queue's contribution to this specific workload's end-to-end throughput is
limited even though it's wait-free in isolation. See the main README's
Performance section for the full discussion, including the TSan-instrumented
run showing the same pattern (1.33x) at lower absolute scale.

### Percentile Calculation

```cpp
std::sort(latencies.begin(), latencies.end());
p50  = latencies[0.500 * (N-1)];
p95  = latencies[0.950 * (N-1)];
p99  = latencies[0.990 * (N-1)];
p999 = latencies[0.999 * (N-1)];
```

---

## EC2 Spot Benchmarking

Automated benchmarking on `c6i.large` (Intel Xeon Ice Lake, 2 vCPU) with CPU isolation for deterministic measurements.

### Isolation techniques

| Technique | Purpose |
|-----------|---------|
| `echo 1 > intel_pstate/no_turbo` | Disable turbo boost — prevents frequency scaling during measurement |
| `echo performance > scaling_governor` | Lock CPU at max frequency — no dynamic scaling |
| `taskset -c 1` | Pin benchmark to single core — avoid scheduler migration |
| `perf stat` | Hardware counters: cache misses, IPC, branch mispredictions |

### Pipeline

```
aws_cloud_init.sh     → One-time: create VPC, security group, key pair, IAM role
run_benchmark.sh      → Launch c6i.large spot instance (falls back to on-demand if
                         Spot capacity is unavailable) with cloud_init.sh as user data
cloud_init.sh         → Install deps, build (-DENABLE_PROFILING=ON), run tests,
                         run benchmark, perf stat, perf record (flame graph),
                         upload to S3, self-terminate
```

The instance self-terminates after uploading results to S3 (a trap on EXIT
handles this on any exit path, not just the happy path), minimizing cost.
Spot pricing typically provides ~70% discount vs on-demand when available.

### Hardware counters captured

```
cache-misses, cache-references    → Cache miss ratio
instructions, cycles              → IPC (instructions per cycle)
L1-dcache-load-misses            → L1 data cache behavior
branch-misses                     → Branch prediction accuracy
```

In practice, on `c6i.large` these all report `<not supported>` — the
instance's virtualization layer doesn't expose PMU counters to the guest.
This is a platform limitation, not a script bug; verified across every run.

### Flame graphs

`perf record -F 999 -g --call-graph dwarf` captures a symbolized profile of
the benchmark (`-DENABLE_PROFILING=ON` keeps the release optimization level
but adds `-g -fno-omit-frame-pointer`), converted to raw `perf script` text
and uploaded to S3. A committed capture and interactive
[speedscope.app](https://www.speedscope.app/) link live in
`orderbook-engine/profiles/` — see the screenshot at the top of the main
README. Samples land inside `OrderBook::add_order` → `std::map::operator[]`
→ `_Rb_tree_insert_and_rebalance`, i.e. real cost in the price-level map's
red-black tree, not scheduler noise — this is why pipeline throughput
trails the isolated per-op latency numbers under sustained load.

`perf` on `PATH` is a version-checking wrapper that refuses to run if the
installed `linux-tools` package version doesn't exactly match the running
kernel — which drifts routinely on EC2 Ubuntu AMIs (apt tracks the latest
kernel in the repo; a given AMI is pinned to whatever it shipped with).
`cloud_init.sh` resolves and calls the real binary under
`/usr/lib/linux-tools/<version>/perf` directly, since the underlying
`perf_event` syscall ABI tolerates the version skew fine.

---

## Testing

52 unit tests across 9 test suites using Google Test:

| Suite | Tests | Coverage |
|-------|-------|----------|
| OrderTest | 3 | Struct initialization, order type validation |
| OrderBookTest | 11 | Crossing orders, partial fills, price-time priority, multi-level sweeps, cancellation |
| OrderQueueTest | 5 | Push/pop, empty/full queue, circular wrap, batch operations |
| MemoryPoolTest | 5 | Allocation correctness, multiple types, stress test (2000 allocations) |
| IntegrationTest | 4 | Pool→queue, queue→book, full pipeline, concurrent SPSC |
| PerformanceTest | 5 | Matching throughput, queue throughput, pool allocation speed, latency thresholds |
| EdgeCaseTest | 13 | Max uint64 IDs, extreme doubles, zero quantity, negative price, cancel-after-fill |
| ConcurrencyTest | 5 | SPSC producer-consumer, multi-queue fan-in, thread-local pool isolation, full pipeline |
| DifferentialTest | 1 | 200 randomized seeds × 300 ops, cross-checked against `NaiveOrderBook` |

Tests override `-fno-exceptions -fno-rtti` with `-fexceptions -frtti` since Google Test requires both.

### Differential testing

Hand-written unit tests only prove the engine handles cases someone
thought to write down. `DifferentialTest` (`tests/test_differential.cpp`)
proves something stronger: `OrderBook` and `NaiveOrderBook`
(`tests/naive_orderbook.hpp`, a deliberately naive O(n) reference matcher
never used in production) independently implement the same
price-time-priority spec and agree on every randomized add/cancel sequence
across 200 seeds × 300 ops, checked after *every* operation — fills,
resting book state (via `bid_book_snapshot()`/`ask_book_snapshot()`), and
cancel results.

This surfaced a real gap in the test itself during development: an early
version generated prices via continuous `std::uniform_real_distribution
<double>`, so two orders essentially never landed on the exact same price
— same-price FIFO tie-breaking was never exercised, and a
deliberately-injected bug (`ask_queue.back()` instead of `.front()`) passed
silently. A discrete 10-tick price ladder, chosen so same-price collisions
actually happen across 300 ops, catches it immediately. A differential
test's power is bounded by its input distribution's coverage of the state
space, not just by having one at all.

### ThreadSanitizer

`ENABLE_TSAN` builds with `-fsanitize=thread -O1 -g -fno-omit-frame-pointer`
instead of the release flags (TSan wants no LTO for accurate, symbolized
stacks) and runs the full suite through it — all 51 tests, including the
5 `ConcurrencyTest` cases exercising the SPSC queue, pass with zero races
reported. On macOS, Apple Clang's bundled TSan runtime crashes at startup
against recent dyld shared-cache formats (macOS 26+); `scripts/run_tsan.sh`
detects this and falls back to Homebrew LLVM automatically. TSan throughput
numbers run ~5–10x slower than native and aren't representative of real
performance.

---

## Latency Sources

Ranked by impact, with mitigations applied:

| Source | Unmitigated cost | Mitigation | Residual |
|--------|-----------------|------------|----------|
| Memory allocation (`malloc`) | 100-500 ns | Slab bump-pointer allocator | ~5 ns |
| Lock contention | 1000+ ns | Lock-free SPSC queue | 0 ns (wait-free) |
| False sharing | 200-500 ns | `alignas(64)` on atomics | 0 ns |
| CPU cache miss (L2→L3) | 10-30 ns | Sequential slab allocation, LTO inlining | ~10 ns (unavoidable on cold data) |
| Memory fence | 10-20 ns | Acquire-release (not seq_cst) | ~10 ns |
| Branch misprediction | 15-20 ns | Predictable buy/sell alternation in hot path | ~5 ns |
| Function call overhead | 5-10 ns | LTO cross-TU inlining | 0 ns |

---

## Scaling Considerations

**Multiple producers:** Deploy N SPSC queues (one per producer thread), round-robin consume in the matching thread. Validated in `ConcurrencyTest::MultipleSPSCQueuesIntoMatchingEngine`.

**Multiple products:** One OrderBook instance per product (e.g., one for ES, one for NQ). Each runs on its own matching thread. This is how real exchanges scale — per-product matching threads, not shared-state concurrency.

**Bottleneck at scale:** At very high throughput, the `std::map` price level lookup (O(log P)) becomes the bottleneck, not the queue or allocator. For production HFT, price levels could be replaced with a fixed-size array indexed by price tick (O(1) lookup) if the price range is bounded. Confirmed empirically, not just theoretically — the EC2 flame graph (`orderbook-engine/profiles/`) shows real sampled time in `std::map::operator[]` → `_Rb_tree_insert_and_rebalance` inside `OrderBook::add_order` under sustained load.

---

## File Reference

| File | Purpose |
|------|---------|
| `include/order.hpp` | Order and Fill struct definitions |
| `include/order_queue.hpp` | SPSC lock-free ring buffer template |
| `include/memory_pool.hpp` | Thread-local slab allocator template |
| `include/orderbook.hpp` | OrderBook class interface |
| `src/orderbook.cpp` | Matching engine implementation |
| `src/order.cpp` | Order translation unit |
| `src/main.cpp` | Demo: pool → queue → match pipeline |
| `benchmarks/latency_test.cpp` | 1M-order latency + pipeline benchmark |
| `tests/` | 52 unit tests across 9 suites |
| `tests/naive_orderbook.hpp` | O(n) reference matcher, differential-testing oracle only |
| `scripts/aws_cloud_init.sh` | AWS infrastructure setup |
| `scripts/run_benchmark.sh` | EC2 spot instance launcher |
| `scripts/cloud_init.sh` | Instance bootstrap + benchmark + perf capture + S3 upload |
| `scripts/run_tsan.sh` | ThreadSanitizer build + test run (macOS: Homebrew LLVM fallback) |
| `profiles/` | Committed EC2 perf capture + speedscope screenshot |
| `CMakeLists.txt` | Build configuration with HFT compiler flags, `ENABLE_TSAN`, `ENABLE_PROFILING` |
