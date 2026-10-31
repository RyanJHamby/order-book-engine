"""Spec bench-pow-capture: the latency benchmark can be run under `pow run` with env info."""

from __future__ import annotations

import os
import stat
import subprocess
from pathlib import Path

import pytest

XFAIL = pytest.mark.xfail(
    strict=True,
    reason="spec:bench-pow-capture (remove this marker when implemented)",
)
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "capture_benchmark.sh"
README = ROOT / "README.md"
ENV_KEYS = [
    "git_sha",
    "git_dirty",
    "uname",
    "cpu_model",
    "logical_cores",
    "compiler",
    "build_type",
    "pmu_counters",
]

# Stand-in for `pow run NAME -- CMD...`: same results/<stamp>-NAME/ layout.
STUB_POW = """#!/bin/bash
[ "$1" = run ] && [ "$3" = -- ] || exit 64
name="$2"; shift 3
dir="$STUB_RESULTS/2026-01-01-0000-$name"
mkdir -p "$dir"
printf '%q ' "$@" > "$dir/cmd.txt"
"$@" 2>&1 | tee "$dir/output.log"
exit "${PIPESTATUS[0]}"
"""
FAKE_BENCH = """#!/bin/bash
echo "=== Pool alloc + add_order latency (1M orders) ==="
echo "  P50:    0.5 us"
exit ${FAKE_BENCH_EXIT:-0}
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def run_capture(tmp_path: Path, pow_bin: str | None = "stub", **extra_env):
    results = tmp_path / "results"
    results.mkdir(exist_ok=True)
    env = dict(os.environ)
    env.update(
        BENCH_BIN=str(_exe(tmp_path / "fake_bench.sh", FAKE_BENCH)),
        SKIP_BUILD="1",
        STUB_RESULTS=str(results),
    )
    if pow_bin == "stub":
        env["POW_BIN"] = str(_exe(tmp_path / "stub_pow.sh", STUB_POW))
    elif pow_bin is not None:
        env["POW_BIN"] = pow_bin
    env.update(extra_env)
    proc = subprocess.run(
        ["bash", str(SCRIPT)], capture_output=True, text=True, env=env, cwd=tmp_path
    )
    return proc, results


def logs(results: Path) -> list[Path]:
    return sorted(results.glob("*-orderbook-latency/output.log"))


@XFAIL
def test_runs_the_benchmark_under_pow_and_keeps_its_output_verbatim(tmp_path):
    proc, results = run_capture(tmp_path)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    (log,) = logs(results)
    text = log.read_text()
    assert (
        "=== Pool alloc + add_order latency (1M orders) ===\n  P50:    0.5 us\n" in text
    )


@XFAIL
def test_environment_block_precedes_the_benchmark_and_has_every_key(tmp_path):
    proc, results = run_capture(tmp_path)
    (log,) = logs(results)
    text = log.read_text()
    assert text.index("=== environment ===") < text.index("=== benchmark ===")
    assert text.index("=== benchmark ===") < text.index("P50:")
    block = text.split("=== environment ===")[1].split("=== benchmark ===")[0]
    values = dict(
        line.split(": ", 1) for line in block.strip().splitlines() if ": " in line
    )
    for key in ENV_KEYS:
        assert key in values, f"missing {key}"
        assert values[key].strip() != "", f"{key} is empty; use 'unknown'"


@XFAIL
def test_pmu_counters_are_reported_unavailable_when_perf_cannot_count(tmp_path):
    # PERF_BIN=false stands in for a small EC2 guest where `perf stat` fails.
    _, results = run_capture(tmp_path, PERF_BIN="false")
    text = logs(results)[0].read_text()
    assert "pmu_counters: unavailable" in text


@XFAIL
def test_pmu_counters_are_reported_available_when_perf_works(tmp_path):
    _, results = run_capture(tmp_path, PERF_BIN="true")
    text = logs(results)[0].read_text()
    assert "pmu_counters: available" in text


@XFAIL
def test_benchmark_exit_code_is_propagated_and_output_still_captured(tmp_path):
    proc, results = run_capture(tmp_path, FAKE_BENCH_EXIT="3")
    assert proc.returncode == 3, proc.stdout + proc.stderr
    assert "P50:    0.5 us" in logs(results)[0].read_text()


@XFAIL
def test_missing_pow_fails_loudly_and_never_runs_the_benchmark(tmp_path):
    proc, results = run_capture(tmp_path, pow_bin="/nonexistent/pow")
    assert proc.returncode == 2, proc.stdout + proc.stderr
    assert "pow" in (proc.stdout + proc.stderr)
    assert not logs(results)


@XFAIL
def test_script_never_touches_the_network_or_aws():
    src = SCRIPT.read_text()
    for banned in ("aws ", "ssh ", "scp ", "curl ", "wget ", "sudo "):
        assert banned not in src, f"capture_benchmark.sh must not use {banned.strip()}"


@XFAIL
def test_readme_documents_the_capture_command_and_the_pmu_limit():
    text = README.read_text()
    assert "capture_benchmark.sh" in text
    assert "PMU" in text
