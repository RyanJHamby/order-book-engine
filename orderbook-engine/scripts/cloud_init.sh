#!/bin/bash
set -euxo pipefail
# tee (not a plain redirect) so output still reaches the serial console --
# `aws ec2 get-console-output` is the only way to see what happened if this
# script fails before it gets far enough to upload logs to S3 itself.
exec > >(tee /home/ubuntu/cloud_init.log) 2>&1

REPO_URL="https://github.com/RyanJHamby/order-book-engine.git"
PROJECT_DIR="/home/ubuntu/order-book-engine/orderbook-engine"
RESULTS_DIR="/home/ubuntu/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

upload_and_shutdown() {
    # Best-effort: get whatever we have into S3 even on failure, then always
    # shut down (self-terminates -- see run_benchmark.sh's launch flags) so a
    # failed run doesn't sit there burning money until the 10-min timeout.
    cp /home/ubuntu/cloud_init.log "$RESULTS_DIR/cloud_init.log" 2>/dev/null || true
    ACCOUNT_ID=$(curl -s http://169.254.169.254/latest/dynamic/instance-identity/document | jq -r '.accountId')
    BUCKET_NAME="orderbook-benchmark-${ACCOUNT_ID}"
    aws s3api head-bucket --bucket "$BUCKET_NAME" 2>/dev/null || aws s3 mb "s3://$BUCKET_NAME" --region us-east-1 2>/dev/null || true
    for f in "$RESULTS_DIR"/*; do
        [ -e "$f" ] || continue
        BASENAME=$(basename "$f" .txt)
        aws s3 cp "$f" "s3://$BUCKET_NAME/${BASENAME}_${TIMESTAMP}.txt" || true
    done
    sudo shutdown -h now
}
trap upload_and_shutdown EXIT

# ---------- System setup ----------
apt-get update -y
apt-get install -y build-essential cmake git awscli jq libgtest-dev \
    linux-tools-common linux-tools-aws

# linux-tools-aws installs whatever kernel-versioned perf build is currently
# in the apt repo (e.g. 6.8.0-1063-aws), which is routinely newer than the
# kernel this AMI actually boots (e.g. 5.15.0-1019-aws) -- Ubuntu's repo and
# a given AMI's baked-in kernel drift out of sync. The `perf` on PATH is a
# wrapper script that refuses to run on a kernel-version mismatch, but the
# perf_event syscall ABI is stable enough that the underlying binary works
# fine regardless -- so resolve and call it directly instead of fighting
# apt to install an exact-version package that may not exist yet.
PERF_BIN=$(find /usr/lib/linux-tools -name perf 2>/dev/null | head -1) || true
if [ -z "$PERF_BIN" ]; then
    echo "No perf binary found under /usr/lib/linux-tools*; perf stat/record will be skipped."
    PERF_BIN="perf"  # let the later calls fail visibly rather than silently
fi
echo "Using perf binary: $PERF_BIN"

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

# ---------- perf permissions ----------
# Allow perf_event access for call-graph recording (default kernel policy
# on Ubuntu restricts this even for root in some configs).
sysctl -w kernel.perf_event_paranoid=-1 || true

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
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PROFILING=ON
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
taskset -c 1 "$PERF_BIN" stat -e cache-misses,cache-references,instructions,cycles,L1-dcache-load-misses,branch-misses \
    ./latency_benchmark >> /dev/null 2>> "$RESULTS_DIR/benchmark_results.txt" || \
    echo "perf stat unavailable (no permissions or kernel support)" >> "$RESULTS_DIR/benchmark_results.txt"

# ---------- perf record for flame graph ----------
# Built with -DENABLE_PROFILING=ON (release flags + -g -fno-omit-frame-pointer)
# so this profiles real hot-path behavior, symbolized. Output is raw
# `perf script` text, which speedscope.app imports directly (drag-and-drop) --
# no need for Brendan Gregg's FlameGraph scripts or an SVG conversion step.
echo "=== perf record (flame graph) ==="
if taskset -c 1 "$PERF_BIN" record -F 999 -g --call-graph dwarf \
    -o "$RESULTS_DIR/perf.data" -- ./latency_benchmark; then
    "$PERF_BIN" script -i "$RESULTS_DIR/perf.data" > "$RESULTS_DIR/perf_script.txt" \
        || echo "perf script conversion failed" > "$RESULTS_DIR/perf_script.txt"
    rm -f "$RESULTS_DIR/perf.data"
else
    echo "perf record failed (no permissions or kernel support)" > "$RESULTS_DIR/perf_script.txt"
fi

echo "=== Done -- upload_and_shutdown trap will handle S3 upload + shutdown ==="
