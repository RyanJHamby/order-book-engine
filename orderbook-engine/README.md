# Low-Latency Order Book Engine

A high-performance C++ order matching engine built for sub-microsecond latency. Implements continuous price-time priority matching with lock-free order ingestion, thread-local memory pools, and automated EC2 Spot benchmarking with P99.9 tail latency profiling.

## Performance

Benchmarked with 1,000,000 orders (alternating buy/sell, random prices, real matching with 773K fills):

| Metric | Value |
|--------|-------|
| P50 latency | 0.21 us |
| P95 latency | 1.5 us |
| P99 latency | 2.6 us |
| P99.9 latency | 5.7 us |
| Pipeline throughput | 2.4M orders/sec |

*Measured with pool allocation + matching in the timed loop. Numbers from local Apple Silicon; EC2 `c6i.large` (Intel Xeon Ice Lake) numbers via `scripts/run_benchmark.sh`.*

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

The system mirrors a real exchange pipeline:

1. **Order ingestion** — Producer threads allocate orders from per-thread slab pools (zero contention, stable pointers) and push them into SPSC lock-free queues.
2. **Matching engine** — A single dedicated thread pops orders from the queue and feeds them to the `OrderBook`. Matching happens inline on `add_order()`: incoming orders cross against resting price levels until filled. No batch step, no locks in the hot path.
3. **Fill reporting** — Each match generates a `Fill` record with buyer/seller IDs, execution price, and quantity.

### Why single-threaded matching?

Lock-free queues eliminate mutex contention at the ingestion boundary. The matching engine itself is single-threaded by design — this avoids synchronization overhead in the critical path and is the standard architecture used by production exchanges (CME Globex, NASDAQ ITCH).

## Key Components

### Matching Engine (`orderbook.hpp`, `orderbook.cpp`)

- **Continuous matching**: Orders match inline in `add_order()`. An aggressive buy sweeps through ask levels lowest-first; an aggressive sell sweeps through bid levels highest-first.
- **Price-time priority**: `std::map<double, std::deque<Order>>` — orders at the same price level are filled FIFO.
- **Partial fills**: Incoming order quantity is decremented against each resting order. Unfilled remainder rests on the book.
- **Order cancellation**: `cancel_order(id)` with O(1) lookup via `unordered_map` index, then removal from the price-level deque.

### SPSC Lock-Free Queue (`order_queue.hpp`)

- Wait-free ring buffer for single-producer, single-consumer use.
- Acquire-release memory ordering: producer acquires consumer's `head`, releases own `tail`; consumer acquires producer's `tail`, releases own `head`.
- `alignas(64)` on head/tail atomics to prevent false sharing across cache lines.
- Usable capacity is N-1 (one sentinel slot for full/empty disambiguation).

### Thread-Local Memory Pool (`memory_pool.hpp`)

- Slab-based allocator: `malloc`s fixed-size slabs of objects. When a slab is exhausted, a new one is allocated. Existing pointers remain valid (no vector-style invalidation).
- `get_thread_local_pool<T>()` returns a `thread_local` instance — each thread gets its own pool with zero contention.
- O(1) bump-pointer allocation within a slab.

### Compiler Flags

```
-O3 -march=native -flto -fno-exceptions -fno-rtti -mavx2
```

- `-flto`: Link-time optimization enables cross-translation-unit inlining of the matching hot path.
- `-fno-exceptions -fno-rtti`: Eliminates exception handling and RTTI overhead. Tests override this (GTest requires exceptions).
- `-mavx2`: Enables AVX2 instruction set on x86_64 targets.

## Building

**Prerequisites:** C++20 compiler, CMake 3.20+, Google Test

```bash
cd orderbook-engine
./scripts/build_and_run_benchmark.sh
```

Or manually:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./latency_benchmark
./tests/orderbook_tests
```

## Testing

51 unit tests across 8 test suites:

| Suite | Tests | Coverage |
|-------|-------|----------|
| OrderTest | 3 | Struct creation, type validation |
| OrderBookTest | 11 | Crossing, partial fills, price-time priority, multi-level sweeps, cancellation |
| OrderQueueTest | 5 | Push/pop, capacity, circular wrap |
| MemoryPoolTest | 5 | Slab allocation, stress (2000 allocs) |
| IntegrationTest | 4 | Pool + queue + matching end-to-end |
| PerformanceTest | 5 | Throughput and latency thresholds |
| EdgeCaseTest | 13 | Extreme values, zero qty, cancel semantics |
| ConcurrencyTest | 5 | SPSC producer-consumer, multi-queue fan-in, pipeline |

```bash
./scripts/run_tests.sh
```

## EC2 Spot Benchmarking

Automated profiling on `c6i.large` (Intel Xeon Ice Lake) Spot instances:

```bash
# One-time setup: creates VPC, security group, key pair, IAM role.
./scripts/aws_cloud_init.sh

# Launch benchmark. Instance self-terminates; results upload to S3.
./scripts/run_benchmark.sh
```

The cloud-init script:
- Disables turbo boost and sets the CPU governor to `performance` for stable measurements
- Pins the benchmark to a single core via `taskset` to avoid scheduler jitter
- Runs `perf stat` for hardware counters (cache misses, IPC, branch mispredicts)
- Captures system info (CPU model, AVX2 flags, kernel version)
- Uploads results to S3 with timestamps

Spot instances run at ~70% discount vs. on-demand. The script reports the exact savings percentage.

## Project Structure

```
orderbook-engine/
├── include/
│   ├── order.hpp            # Order and Fill structs
│   ├── orderbook.hpp        # Matching engine interface
│   ├── order_queue.hpp      # SPSC lock-free ring buffer
│   └── memory_pool.hpp      # Thread-local slab allocator
├── src/
│   ├── main.cpp             # Demo: pool -> queue -> match pipeline
│   ├── order.cpp            # Order translation unit
│   └── orderbook.cpp        # Matching engine implementation
├── benchmarks/
│   └── latency_test.cpp     # 1M-order benchmark with percentiles
├── tests/                   # 51 Google Test cases (8 suites)
├── scripts/
│   ├── aws_cloud_init.sh    # One-time AWS infrastructure setup
│   ├── run_benchmark.sh     # EC2 Spot instance launcher
│   ├── cloud_init.sh        # EC2 instance bootstrap + benchmark
│   ├── build_and_run_benchmark.sh  # Local build + benchmark
│   └── run_tests.sh         # Unit test runner
└── CMakeLists.txt           # Build configuration
```
