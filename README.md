# Low-Latency Order Book Engine

This project implements a minimal, high-performance order book engine in C++ designed to explore the foundations of low-latency trading systems. It includes a nanosecond-level order matcher skeleton, placeholders for SIMD optimizations, and a benchmarking harness to measure throughput and latency. The project can be executed locally or on AWS EC2 spot instances, with automated build and benchmarking to minimize operational overhead and costs.

---

## Build and Run Locally

**Prerequisites:**
- C++20 compiler
- CMake
- GNU Make
- Google Test (for unit tests)

**Steps:**
```bash
cd orderbook-engine
chmod +x scripts/build_and_run_benchmark.sh
./scripts/build_and_run_benchmark.sh
```

This script performs the following:
- Cleans and creates a fresh build directory
- Configures the project with CMake
- Builds the order book engine and benchmark using parallel jobs
- Runs the main order book executable for validation
- Executes the latency benchmark and saves results in `build/benchmark_results.txt`

## Running Unit Tests

The project includes comprehensive unit tests using Google Test framework:

```bash
chmod +x scripts/run_tests.sh
./scripts/run_tests.sh
```

**Test Coverage:**
- **Order Tests**: Order creation, type validation, comparison operations
- **OrderBook Tests**: Order addition, matching, stress testing with 1000 orders
- **LockFreeQueue Tests**: Push/pop operations, circular behavior, multiple order handling
- **MemoryPool Tests**: Thread-local allocation, stress testing with 2000 allocations

All tests run in Debug mode for better error detection and validation.

## AWS EC2 Spot Benchmarking

The project supports automated benchmarking on EC2 spot instances:

1. Ensure `scripts/cloud_init.sh` points to the correct repository URL
2. Launch an EC2 spot instance using `scripts/run_benchmark.sh`:

```bash
chmod +x scripts/run_benchmark.sh
./scripts/run_benchmark.sh
```

This process:
- Launches a spot instance with the specified AMI, instance type, key pair, and security group
- Cloud-init installs dependencies (build-essential, cmake, git, perf)
- Clones the repository and executes `build_and_run_benchmark.sh`
- Runs the benchmark and logs output to `/home/ubuntu/benchmark_results.txt`
- Shuts down the instance automatically after completion
- Logs can be inspected in `/home/ubuntu/cloud-init.log`

## Benchmarking

- Simulates one million orders with alternating buy and sell sides
- Measures average microseconds per match
- Designed for low-latency performance measurement with a clear path to optimizations
- Results are reproducible locally or on cloud infrastructure

## Design Rationale

- **Low-latency architecture**: Minimal memory allocation, inlined matching, skeleton for SIMD/NUMA-aware optimizations
- **Separation of concerns**: Ingestion, matching, and benchmarking are decoupled to mirror exchange-like pipelines
- **Automation**: Cloud-init and spot instances allow repeatable benchmarking at low cost
- **HFT mindset**: The codebase provides a foundation to explore real-time optimizations, including multi-threading, lock-free data structures, and CPU-specific vectorization

## Optimization Roadmap

- Implement lock-free queues for order ingestion
- SIMD vectorization for price matching (`simd_price_match`)
- NUMA-aware memory allocation for multi-threaded workloads
- Realistic market simulation with order cancellations and partial fills
- Metrics and logging for latency spikes and throughput bottlenecks

## Project Structure

```
orderbook-engine/
├── include/
│   ├── order.hpp           # Order data structure
│   ├── orderbook.hpp       # OrderBook class interface
│   ├── order_queue.hpp     # Lock-free queue template
│   └── memory_pool.hpp    # Thread-local memory pool
├── src/
│   ├── main.cpp           # Main executable
│   ├── order.cpp          # Order implementation
│   ├── orderbook.cpp      # OrderBook implementation
│   └── memory_pool.cpp    # Memory pool implementation
├── benchmarks/
│   └── latency_test.cpp   # Performance benchmark
├── tests/
│   ├── test_order.cpp     # Order struct unit tests
│   ├── test_orderbook.cpp # OrderBook class unit tests
│   ├── test_order_queue.cpp # LockFreeQueue unit tests
│   ├── test_memory_pool.cpp # MemoryPool unit tests
│   └── CMakeLists.txt     # Test build configuration
├── scripts/
│   ├── run_benchmark.sh   # AWS EC2 spot instance launcher
│   ├── cloud_init.sh      # EC2 instance initialization
│   ├── build_and_run_benchmark.sh # Local build and benchmark
│   └── run_tests.sh       # Unit test runner
├── CMakeLists.txt         # Build configuration
└── README.md              # This file
```

## References

- [AWS EC2 Spot Instances](https://aws.amazon.com/ec2/spot/) for cost-efficient benchmarking
- [Modern C++20 features](https://en.cppreference.com/w/cpp/20) for performance-critical systems
- [SIMD/AVX2 intrinsics](https://software.intel.com/sites/landingpage/IntrinsicsGuide/) for numerical optimizations
- [Cloud-init documentation](https://cloud-init.io/)

