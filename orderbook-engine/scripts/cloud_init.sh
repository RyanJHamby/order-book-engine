#!/bin/bash
set -euo pipefail
exec > /home/ubuntu/cloud_init.log 2>&1

REPO_URL="https://github.com/RyanJHamby/order-book-engine.git"
PROJECT_DIR="/home/ubuntu/order-book-engine/orderbook-engine"
RESULTS_DIR="/home/ubuntu/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

# ---------- System setup ----------
apt-get update -y
apt-get install -y build-essential cmake git linux-tools-common linux-tools-aws \
    linux-tools-$(uname -r) awscli jq libgtest-dev

# ---------- Capture system info ----------
{
    echo "=== System Info ==="
    echo "Date:     $(date -u)"
    echo "Instance: $(curl -s http://169.254.169.254/latest/meta-data/instance-type)"
    echo "AMI:      $(curl -s http://169.254.169.254/latest/meta-data/ami-id)"
    echo "Region:   $(curl -s http://169.254.169.254/latest/meta-data/placement/region)"
    echo "Kernel:   $(uname -r)"
    echo "CPU:      $(lscpu | grep 'Model name' | sed 's/.*: *//')"
    echo "Cores:    $(nproc)"
    echo "RAM:      $(free -h | awk '/Mem:/{print $2}')"
    echo ""
    echo "=== CPU Flags ==="
    grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -E 'avx|sse|bmi' | sort | tr '\n' ' '
    echo ""
} > "$RESULTS_DIR/system_info.txt"

# ---------- CPU isolation for benchmarking ----------
# Disable turbo boost for consistent measurements.
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "Turbo boost disabled (intel_pstate)"
fi

# Set CPU governor to performance.
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo performance > "$cpu"
done

# ---------- Clone and build ----------
cd /home/ubuntu
git clone "$REPO_URL" || echo "Repo already exists"
cd "$PROJECT_DIR"

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# ---------- Run tests ----------
echo "=== Running unit tests ===" | tee "$RESULTS_DIR/test_results.txt"
./tests/orderbook_tests 2>&1 | tee -a "$RESULTS_DIR/test_results.txt" || true

# ---------- Run benchmark (pinned to core 1 to avoid scheduler jitter) ----------
echo "=== Running latency benchmark ===" | tee "$RESULTS_DIR/benchmark_results.txt"
taskset -c 1 ./latency_benchmark 2>&1 | tee -a "$RESULTS_DIR/benchmark_results.txt"

# ---------- perf stat for hardware counters ----------
echo "" >> "$RESULTS_DIR/benchmark_results.txt"
echo "=== perf stat ===" >> "$RESULTS_DIR/benchmark_results.txt"
taskset -c 1 perf stat -e cache-misses,cache-references,instructions,cycles,L1-dcache-load-misses,branch-misses \
    ./latency_benchmark >> /dev/null 2>> "$RESULTS_DIR/benchmark_results.txt" || \
    echo "perf stat unavailable (no permissions or kernel support)" >> "$RESULTS_DIR/benchmark_results.txt"

# ---------- Upload to S3 ----------
ACCOUNT_ID=$(curl -s http://169.254.169.254/latest/dynamic/instance-identity/document | jq -r '.accountId')
BUCKET_NAME="orderbook-benchmark-${ACCOUNT_ID}"

aws s3api head-bucket --bucket "$BUCKET_NAME" 2>/dev/null || \
    aws s3 mb "s3://$BUCKET_NAME"

for f in "$RESULTS_DIR"/*; do
    BASENAME=$(basename "$f" .txt)
    aws s3 cp "$f" "s3://$BUCKET_NAME/${BASENAME}_${TIMESTAMP}.txt"
done

echo "Results uploaded to s3://$BUCKET_NAME/"

# ---------- Shutdown ----------
sudo shutdown -h now
