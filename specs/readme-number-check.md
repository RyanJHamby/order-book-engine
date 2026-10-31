---
id: readme-number-check
goal: A stdlib-only script fails (exit 1) and names every README performance number (us/ns/ms latency, orders/sec throughput) that does not trace to a successful captured run in results/*/output.log.
acceptance:
  - python3 -m pytest orderbook-engine/pytests/test_check_readme_numbers.py --runxfail -q
  - python3 -m pytest orderbook-engine/pytests/test_check_readme_numbers.py -q
scope:
  - orderbook-engine/scripts/check_readme_numbers.py
  - orderbook-engine/pytests/test_check_readme_numbers.py
non_goals:
  - do not change any assertion in the test file; only delete the `XFAIL` constant and the `@XFAIL` decorator lines (the plain pytest acceptance command fails while the strict xfail marker remains and the work is done)
  - do not edit README.md, orderbook-engine/README.md or design.md; the real READMEs are expected to FAIL this checker today and fixing that is the owner's decision (capture runs or delete the numbers)
  - do not create or fabricate anything under results/
  - no third-party dependencies (stdlib only); do not wire the script into CI, a git hook or CMake
  - do not try to verify the hardware named next to a number; only that the number traces to a log
risk_tier: 1
max_runs: 3
autonomy: inherit
status: ready
issue: 5
---

## Why
The owner's rule is that no number is quoted unless it comes from a captured run (`pow run NAME -- CMD` writes
`results/<date>-NAME/{cmd.txt,env.txt,output.log}`). `README.md` and `orderbook-engine/README.md` currently quote P50/P95/P99/P99.9
latencies and orders/sec figures with no `results/` directory in the repo at all. This spec builds the tripwire; it does not
change the READMEs.

## What to build: `orderbook-engine/scripts/check_readme_numbers.py`
`python3 check_readme_numbers.py --readme PATH [--readme PATH ...] --results DIR`

- Exit 0: every claim is traceable (or there are no claims). Exit 1: at least one claim is not traceable. Argument errors may
  exit 2 (argparse default). Never exit 1 for a usage error.
- Print one line per untraceable claim to stdout: `<readme file name>:<line number>: <number> (<claim text as written>) not traceable`.
  The test requires the file name with line (`README.md:1`) and the claim text (`2.6 us`, `1.43M orders/sec`).

### What counts as a claim (everything else is ignored)
A number, or a range `A-B` / `A–B` (hyphen or en dash), directly followed by one of these units, case sensitive:
`us`, `ns`, `ms`, `orders/sec`, `M orders/sec` (space optional: `1.42M orders/sec`). Plain counts ("52 tests", "200 seeds",
"1,000,000 orders", "C++20", "Python 3.13") have no such unit and are not claims. Both ends of a range are separate claims.
Thousands commas are allowed in numbers.

### What backs a claim
Scan `DIR/*/` run folders. A folder counts only if it has `output.log` and `env.txt` and `env.txt` contains the line `exit_code: 0`.
In each `output.log`, find `<number> <unit>` occurrences using the same units (the benchmark prints e.g. `P50:    0.208 us`,
`Total:  704.1 ms`, `Throughput: 1423456 orders/sec`; see `orderbook-engine/benchmarks/latency_test.cpp`).
- A README claim is backed when some log value of the SAME unit family, rounded half-up to the number of decimal places the README
  wrote, equals the README number. `0.21 us` is backed by a logged `0.208 us`; `3.09 us` by `3.09 us`.
- `M orders/sec` is compared against logged `orders/sec` divided by 1,000,000 (`1.42M orders/sec` backed by `1423456 orders/sec`).
  Use `decimal.Decimal`, not floats, so the rounding is exact.
- Units never cross: a logged `0.208 us` does not back `0.21 ms`.

## Test notes
`orderbook-engine/pytests/test_check_readme_numbers.py` builds its own fixtures in `tmp_path` and calls the script through
`subprocess`. Run pytest from the repo root. Lint: `ruff check orderbook-engine` and `ruff format --check orderbook-engine`.

## Log
- 2026-10-30 draft (provisional handshake; repo not onboarded to autopilot yet)
- 2026-10-31 ready, issue #5 (handshake was provisional; repo not yet onboarded with ap init; waiting for the owner's agent-ready label)
