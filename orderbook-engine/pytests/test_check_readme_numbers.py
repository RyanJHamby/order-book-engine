"""Spec readme-number-check: a README performance number must trace to a captured run."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

XFAIL = pytest.mark.xfail(
    strict=True,
    reason="spec:readme-number-check (remove this marker when implemented)",
)
SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "check_readme_numbers.py"

BENCH_LOG = """\
=== Pool alloc + add_order latency (1M orders) ===
  Orders: 1000000  Fills: 773000  Slabs: 4
  Mean:   0.4 us
  P50:    0.208 us
  P95:    1.26 us
  P99:    1.95 us
  P99.9:  3.09 us
  Max:    812.5 us

=== Full pipeline: pool -> queue -> match (1M orders) ===
  Orders: 1000000  Fills: 773000
  Total:  704.1 ms
  Throughput: 1423456 orders/sec
"""


def make_run(
    results: Path, name: str, log: str = BENCH_LOG, exit_code: int = 0
) -> None:
    run = results / f"2026-01-01-0000-{name}"
    run.mkdir(parents=True)
    (run / "output.log").write_text(log)
    (run / "cmd.txt").write_text("./latency_benchmark\n")
    (run / "env.txt").write_text(
        f"cpu: test\nexit_code: {exit_code}\nwall_seconds: 1\n"
    )


def run_check(tmp_path: Path, readme_text: str, with_run: bool = True, **run_kw):
    readme = tmp_path / "README.md"
    readme.write_text(readme_text)
    results = tmp_path / "results"
    results.mkdir()
    if with_run:
        make_run(results, "bench", **run_kw)
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--readme",
            str(readme),
            "--results",
            str(results),
        ],
        capture_output=True,
        text=True,
    )


@XFAIL
def test_numbers_that_appear_in_a_captured_log_pass(tmp_path):
    readme = "P50 is 0.21 us and P99.9 is 3.09 us. Throughput is 1.42M orders/sec.\n"
    r = run_check(tmp_path, readme)
    assert r.returncode == 0, r.stdout + r.stderr


@XFAIL
def test_a_number_missing_from_every_log_fails_and_is_named(tmp_path):
    r = run_check(tmp_path, "| P99 latency | 2.6 us |\n")
    assert r.returncode == 1, r.stdout + r.stderr
    assert "2.6 us" in r.stdout
    assert "README.md:1" in r.stdout


@XFAIL
def test_claims_with_no_captured_runs_at_all_fail(tmp_path):
    r = run_check(tmp_path, "P50 is 0.21 us.\n", with_run=False)
    assert r.returncode == 1, r.stdout + r.stderr


@XFAIL
def test_every_end_of_a_range_must_be_backed(tmp_path):
    (tmp_path / "a").mkdir()
    ok = run_check(tmp_path / "a", "P99.9 is 3.09-3.09 us.\n")
    assert ok.returncode == 0, ok.stdout + ok.stderr
    (tmp_path / "b").mkdir()
    bad = run_check(tmp_path / "b", "P99.9 is 3.09–3.23 us.\n")
    assert bad.returncode == 1, bad.stdout + bad.stderr
    assert "3.23" in bad.stdout


@XFAIL
def test_millions_of_orders_per_sec_is_compared_to_the_raw_log_value(tmp_path):
    bad = run_check(tmp_path, "Throughput is 1.43M orders/sec.\n")
    assert bad.returncode == 1, bad.stdout + bad.stderr
    assert "1.43M orders/sec" in bad.stdout


@XFAIL
def test_numbers_without_a_performance_unit_are_ignored(tmp_path):
    readme = "52 tests, 200 randomized seeds, 1,000,000 orders, C++20, Python 3.13.\n"
    r = run_check(tmp_path, readme, with_run=False)
    assert r.returncode == 0, r.stdout + r.stderr


@XFAIL
def test_a_failed_run_does_not_back_a_number(tmp_path):
    r = run_check(tmp_path, "P50 is 0.21 us.\n", exit_code=1)
    assert r.returncode == 1, r.stdout + r.stderr


@XFAIL
def test_unit_must_match_not_just_the_digits(tmp_path):
    # 0.208 exists in the log as microseconds; claiming it as milliseconds is not backed.
    r = run_check(tmp_path, "P50 is 0.21 ms.\n")
    assert r.returncode == 1, r.stdout + r.stderr
