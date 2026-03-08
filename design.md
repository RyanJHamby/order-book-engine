# Design Document: Low-Latency Order Book Engine

## Overview

A high-performance C++20 order matching engine built for sub-microsecond latency. Implements continuous price-time priority matching with lock-free order ingestion, thread-local memory pools, and automated EC2 Spot benchmarking with P99.9 tail latency profiling.

### Performance

Benchmarked with 1,000,000 orders (alternating buy/sell, random prices, real matching with 773K fills):

| Metric | Value |
|--------|-------|
| P50 latency | 0.21 μs |
| P95 latency | 1.5 μs |
| P99 latency | 2.6 μs |
| P99.9 latency | 5.7 μs |
| Pipeline throughput | 2.4M orders/sec |

Measured with pool allocation + matching in the timed loop. Numbers from local Apple Silicon; EC2 `c6i.large` (Intel Xeon Ice Lake) numbers via `scripts/run_benchmark.sh`.

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

Measures end-to-end including cross-thread communication overhead. Typical result: ~2.4M orders/sec.

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
run_benchmark.sh      → Launch c6i.large spot instance with cloud_init.sh as user data
cloud_init.sh         → Install deps, build, run tests, run benchmark, upload to S3, self-terminate
```

The instance self-terminates after uploading results to S3, minimizing cost. Spot pricing typically provides ~70% discount vs on-demand.

### Hardware counters captured

```
cache-misses, cache-references    → Cache miss ratio
instructions, cycles              → IPC (instructions per cycle)
L1-dcache-load-misses            → L1 data cache behavior
branch-misses                     → Branch prediction accuracy
```

---

## Testing

51 unit tests across 8 test suites using Google Test:

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

Tests override `-fno-exceptions -fno-rtti` with `-fexceptions -frtti` since Google Test requires both.

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

**Bottleneck at scale:** At very high throughput, the `std::map` price level lookup (O(log P)) becomes the bottleneck, not the queue or allocator. For production HFT, price levels could be replaced with a fixed-size array indexed by price tick (O(1) lookup) if the price range is bounded.

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
| `tests/` | 51 unit tests across 8 suites |
| `scripts/aws_cloud_init.sh` | AWS infrastructure setup |
| `scripts/run_benchmark.sh` | EC2 spot instance launcher |
| `scripts/cloud_init.sh` | Instance bootstrap + benchmark + S3 upload |
| `CMakeLists.txt` | Build configuration with HFT compiler flags |
