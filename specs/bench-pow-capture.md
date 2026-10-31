---
id: bench-pow-capture
goal: One script runs the latency benchmark under `pow run`, so results/<date>-orderbook-latency/output.log holds an environment block (including whether PMU counters work) followed by the benchmark's raw output.
acceptance:
  - python3 -m pytest orderbook-engine/pytests/test_capture_benchmark.py --runxfail -q
  - python3 -m pytest orderbook-engine/pytests/test_capture_benchmark.py -q
scope:
  - orderbook-engine/scripts/capture_benchmark.sh
  - orderbook-engine/pytests/test_capture_benchmark.py
  - orderbook-engine/README.md
non_goals:
  - do not change any assertion in the test file; only delete the `XFAIL` constant and the `@XFAIL` decorator lines (the plain pytest acceptance command fails while the strict xfail marker remains and the work is done)
  - do not touch AWS, SSH, S3, EC2, curl or sudo; do not edit cloud_init.sh, run_benchmark.sh or aws_cloud_init.sh; do not run anything on EC2
  - do not run the real benchmark or `pow run` in the tests (they use a stub pow and a fake benchmark)
  - do not add, change or "update" any benchmark number in any README; the README edit is one short usage section with no figures
  - do not change latency_test.cpp or any C++ source, and do not add CPU pinning (taskset) in this spec
  - do not create anything under results/ yourself; only the owner records real runs
risk_tier: 1
max_runs: 3
autonomy: inherit
status: draft
---

## Why
`README.md` quotes P50/P99.9 latency and orders/sec for EC2 `c6i.large`, but the repo has no `results/` directory: nothing
dated or raw backs those numbers. `pow run NAME -- CMD...` (from `~/.local/bin/pow`) saves `cmd.txt`, `env.txt` (machine
info, exit code, wall time) and `output.log` (raw output) under `results/<YYYY-MM-DD-HHMM>-NAME/`. `env.txt` does not say
how the binary was built or whether hardware counters work, so the wrapper prints that into the log itself.

## What to build: `orderbook-engine/scripts/capture_benchmark.sh` (bash, `set -uo pipefail`)
Normal use, from anywhere inside the repo, after building `latency_benchmark` with `-DCMAKE_BUILD_TYPE=Release`:
`orderbook-engine/scripts/capture_benchmark.sh`.

Environment variables (the tests set them; defaults are for real use):
- `POW_BIN` (default `pow`): if not executable or not on PATH, print a message containing `pow` to stderr and exit 2 before
  doing anything else.
- `BENCH_BIN` (default `<script dir>/../build/latency_benchmark`).
- `SKIP_BUILD` (default unset): when unset, first run
  `cmake -S <orderbook-engine dir> -B <orderbook-engine dir>/build -DCMAKE_BUILD_TYPE=Release` then
  `cmake --build <orderbook-engine dir>/build --target latency_benchmark`; when `1`, skip the build.
- `PERF_BIN` (default `perf`).

Flow: the script re-invokes itself as `"$POW_BIN" run orderbook-latency -- bash "$0" --inner`. The `--inner` leg prints, to
stdout, exactly this shape and then `exec`s `$BENCH_BIN` so its output follows unmodified and its exit code propagates:

```
=== environment ===
key: value        (one per line, keys below, never empty: write the word unknown when unreadable)
=== benchmark ===
<raw benchmark output>
```

Keys: `git_sha` (full HEAD sha), `git_dirty` (`true`/`false`), `uname` (`uname -srm`), `cpu_model` (macOS:
`sysctl -n machdep.cpu.brand_string`; Linux: first `model name` in /proc/cpuinfo), `logical_cores` (`getconf _NPROCESSORS_ONLN`),
`compiler` (first line of `${CXX:-c++} --version`), `build_type` (`CMAKE_BUILD_TYPE` from `build/CMakeCache.txt`),
`pmu_counters`.

`pmu_counters` is `available` when `"$PERF_BIN" stat -e cycles true` exits 0 and `unavailable` otherwise (including perf not
installed). Background the README already states: small EC2 instances such as `c6i.large` do not expose PMU counters to the
guest, so `perf stat` reports `<not supported>`; macOS has no `perf`. The log must say so rather than leave the reader guessing.

## README (`orderbook-engine/README.md`)
Add one short section, e.g. "Capturing a benchmark run", showing `scripts/capture_benchmark.sh`, saying results land in
`results/<date>-orderbook-latency/`, and noting that PMU counters (`pmu_counters: unavailable`) are expected on small EC2
instances. The test requires the strings `capture_benchmark.sh` and `PMU`. Quote no number.

## Hard facts
- Existing scripts live in `orderbook-engine/scripts/` and use `#!/bin/bash`; match their style. Make the new file executable.
- The `build/` directory is git-ignored. The CMake target name is `latency_benchmark`.
- `pow` is not available in every environment, which is why tests inject `POW_BIN`.
- Python tests: run pytest from the repo root; lint with `ruff check orderbook-engine` and `ruff format --check orderbook-engine`.

## Log
- 2026-10-30 draft (provisional handshake; repo not onboarded to autopilot yet)
