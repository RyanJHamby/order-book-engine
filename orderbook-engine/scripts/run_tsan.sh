#!/bin/bash
# Build and run the test suite + latency benchmark under ThreadSanitizer.
#
# On macOS, Apple Clang's bundled TSan runtime crashes at startup against
# recent dyld shared cache formats (macOS 26+). If Homebrew LLVM is present
# we use its clang++ instead, which ships a current sanitizer runtime.
set -e

echo "Running OrderBook Engine under ThreadSanitizer"
echo "================================================"

CXX_OVERRIDE=()
if [[ "$(uname)" == "Darwin" ]] && [[ -x /opt/homebrew/opt/llvm/bin/clang++ ]]; then
    echo "Using Homebrew LLVM (Apple Clang's TSan runtime is broken on recent macOS)."
    CXX_OVERRIDE=(-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang
                  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++)
fi

rm -rf build-tsan
mkdir -p build-tsan
cd build-tsan

echo "Configuring project..."
cmake .. -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug "${CXX_OVERRIDE[@]}"

echo "Building project..."
cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo "Running tests under TSan..."
./tests/orderbook_tests

echo "Running latency benchmark under TSan (numbers are NOT representative of"
echo "real throughput — TSan instrumentation costs ~5-10x; use scripts/run_benchmark.sh"
echo "for real numbers)..."
./latency_benchmark

echo "ThreadSanitizer run completed with no reported races."
