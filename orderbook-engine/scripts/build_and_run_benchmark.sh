#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "=== Building OrderBook Engine ==="
rm -rf build
mkdir build
cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

echo ""
echo "=== Running orderbook demo ==="
./orderbook || true

echo ""
echo "=== Running unit tests ==="
./tests/orderbook_tests || true

echo ""
echo "=== Running latency benchmark ==="
./latency_benchmark | tee benchmark_results.txt
