#!/usr/bin/env python3
# ruff: noqa
# (kit-managed file: host-repo lint rules do not apply)
"""Autopilot budget gate and picker. Costs zero tokens; runs before any agent job.

Copied into each repo as .autopilot/gate.py. State comes from GitHub itself
(recent workflow runs, open PRs), so there is no ledger file for an agent to forge.

  gate.py check  --role implementer [--hard]   -> writes allow/model/effort/max_turns/remaining to $GITHUB_OUTPUT
  (counts only agent-exec.yml runs, so gate denials never consume budget)
  gate.py dispatch [--issue N]                  -> gate each issue, fire agent-exec.yml for allowed ones
  gate.py pick   --limit N                      -> prints issue numbers eligible for the nightly run
Requires `gh` with GH_TOKEN, and PyYAML.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from dataclasses import dataclass

DEFAULTS = {
    "models": {"triage": "haiku", "default": "sonnet", "hard": "opus"},
    "effort": {"triage": "low", "default": "medium", "hard": "high"},
    "max_turns": {"standard": 25, "yolo": 60, "hard": 30, "triage": 5, "reviewer": 10},
    "runs_per_day": {"A": 2, "B": 1, "C": 0},
    "hard_runs_per_day": 1,
    "max_open_agent_prs": 3,
    "breaker_failures": 2,
}
SKIP_LABELS = {"hold", "needs-human", "blocked", "agent-working"}


@dataclass
class Decision:
    allow: bool
    reason: str
    model: str = ""
    effort: str = ""
    max_turns: int = 0
    remaining: int = 0  # runs left in today's cap, before this one

    def outputs(self) -> dict[str, str]:
        return {
            "allow": str(self.allow).lower(),
            "reason": self.reason,
            "model": self.model,
            "effort": self.effort,
            "max_turns": str(self.max_turns),
            "remaining": str(self.remaining),
        }


def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in (over or {}).items():
        out[k] = _merge(base[k], v) if isinstance(v, dict) and isinstance(base.get(k), dict) else v
    return out


def decide(
    cfg: dict, runs: list[dict], open_agent_prs: int, *, role: str, hard: bool = False, now: dt.datetime | None = None
) -> Decision:
    """Pure decision function. `runs` = recent completed/queued runs of the agent workflow,
    newest first, each {"created_at": iso8601, "conclusion": str|None, "status": str, "hard": bool}."""
    now = now or dt.datetime.now(dt.timezone.utc)
    c = _merge(DEFAULTS, cfg.get("budget", {}) or {})
    mode = cfg.get("mode", "dormant")
    tier = cfg.get("tier", "C")
    if mode == "dormant":
        return Decision(False, "repo is dormant")
    since = now - dt.timedelta(hours=24)
    # skipped/cancelled runs never reached the model, so they must not eat the daily budget
    recent = [
        r
        for r in runs
        if r.get("conclusion") not in ("skipped", "cancelled")
        and dt.datetime.fromisoformat(r["created_at"].replace("Z", "+00:00")) >= since
    ]
    cap = int(c["runs_per_day"].get(tier, 0))
    if len(recent) >= cap:
        return Decision(False, f"daily run cap reached ({len(recent)}/{cap}, tier {tier})")
    if open_agent_prs >= int(c["max_open_agent_prs"]) and role == "implementer":
        return Decision(False, f"{open_agent_prs} agent PRs already open (cap {c['max_open_agent_prs']}); review them first")
    n = int(c["breaker_failures"])
    finished = [r for r in runs if r.get("status") == "completed"][:n]
    if len(finished) == n and all(r.get("conclusion") in ("failure", "timed_out", "startup_failure") for r in finished):
        return Decision(False, f"circuit breaker: last {n} runs failed; fix or re-enable manually")
    if hard:
        if tier != "A":
            hard = False
        elif sum(1 for r in recent if r.get("hard")) >= int(c["hard_runs_per_day"]):
            return Decision(False, "daily opus (hard) run already used")
    if role == "triage":
        key_m, key_e, turns = "triage", "triage", c["max_turns"]["triage"]
    elif role == "reviewer":
        key_m, key_e, turns = "default", "default", c["max_turns"]["reviewer"]
    elif hard:
        key_m, key_e, turns = "hard", "hard", c["max_turns"]["hard"]
    else:
        key_m, key_e, turns = "default", "default", c["max_turns"].get(mode, 25)
    return Decision(True, "ok", c["models"][key_m], c["effort"][key_e], int(turns), remaining=cap - len(recent))


def pick_issues(issues: list[dict], open_pr_branches: list[str], limit: int) -> list[int]:
    """Oldest agent-ready issues first (p0 before p1 before unlabeled), skipping held/in-flight ones."""

    def prio(i: dict) -> int:
        names = {ln["name"] for ln in i.get("labels", [])}
        return 0 if "p0" in names else 1 if "p1" in names else 2

    eligible = []
    for i in issues:
        names = {ln["name"] for ln in i.get("labels", [])}
        if "agent-ready" not in names or names & SKIP_LABELS:
            continue
        if any(b.startswith(f"agent/{i['number']}-") for b in open_pr_branches):
            continue
        eligible.append(i)
    eligible.sort(key=lambda i: (prio(i), i["createdAt"]))
    return [i["number"] for i in eligible[:limit]]


def cmd_dispatch(cfg: dict, issue: int | None, *, gh=None, run=None, sleep=None, workflow: str = "agent-exec.yml") -> int:
    """Decide per issue and fire agent-exec.yml for each allowed one. Returns number dispatched."""
    import time

    gh = gh or _gh
    run = run or (lambda *a: subprocess.run(list(a), check=True))
    sleep = sleep or time.sleep
    dispatched = 0
    queue = [issue] if issue else None
    while True:
        runs_raw = gh(
            "run", "list", "--workflow", workflow, "--limit", "30", "--json", "createdAt,conclusion,status,displayTitle"
        )
        runs = [
            {
                "created_at": r["createdAt"],
                "conclusion": r.get("conclusion"),
                "status": r["status"],
                "hard": "[hard]" in (r.get("displayTitle") or ""),
            }
            for r in runs_raw
        ]
        prs = gh("pr", "list", "--state", "open", "--limit", "100", "--json", "headRefName")
        branches = [p_["headRefName"] for p_ in prs]
        n_agent = sum(1 for b in branches if b.startswith("agent/"))
        if queue is None:
            probe = decide(cfg, runs, n_agent, role="implementer")
            if not probe.allow:
                print(f"::notice::autopilot gate closed: {probe.reason}")
                return dispatched
            issues = gh(
                "issue",
                "list",
                "--label",
                "agent-ready",
                "--state",
                "open",
                "--limit",
                "100",
                "--json",
                "number,labels,createdAt",
            )
            queue = pick_issues(issues, branches, probe.remaining)
        if not queue:
            return dispatched
        n = queue.pop(0)
        info = gh("issue", "view", str(n), "--json", "labels")
        names = {ln["name"] for ln in info.get("labels", [])}
        if names & SKIP_LABELS:
            print(f"::notice::issue #{n} is held ({sorted(names & SKIP_LABELS)}); skipping")
            if not queue:
                return dispatched
            continue
        d = decide(cfg, runs, n_agent, role="implementer", hard="hard" in names)
        if not d.allow:
            print(f"::notice::autopilot gate closed for #{n}: {d.reason}")
            return dispatched
        run(
            "gh",
            "workflow",
            "run",
            workflow,
            "-f",
            f"issue={n}",
            "-f",
            f"model={d.model}",
            "-f",
            f"effort={d.effort}",
            "-f",
            f"max_turns={d.max_turns}",
            "-f",
            f"hard={str(d.model == 'opus').lower()}",
        )
        print(f"dispatched #{n}: {d.model}/{d.effort}, {d.max_turns} turns")
        dispatched += 1
        if not queue:
            return dispatched
        sleep(10)


def _gh(*args: str) -> object:
    out = subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout
    return json.loads(out) if out.strip() else []


def _load_cfg(path: str) -> dict:
    import yaml  # type: ignore

    with open(path) as fh:
        return yaml.safe_load(fh) or {}


def _emit(d: Decision) -> None:
    for k, v in d.outputs().items():
        print(f"{k}={v}")
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as fh:
            for k, v in d.outputs().items():
                fh.write(f"{k}={v}\n")
    if not d.allow:
        print(f"::notice::autopilot gate closed: {d.reason}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--role", default="implementer", choices=["implementer", "reviewer", "triage"])
    c.add_argument("--hard", action="store_true")
    c.add_argument("--workflow", default="agent-exec.yml")
    c.add_argument("--config", default=".autopilot/repo.yaml")
    dp = sub.add_parser("dispatch")
    dp.add_argument("--issue", type=int, default=None, help="dispatch this issue; default = pick from agent-ready")
    dp.add_argument("--config", default=".autopilot/repo.yaml")
    p = sub.add_parser("pick")
    p.add_argument("--limit", type=int, default=1)
    p.add_argument("--config", default=".autopilot/repo.yaml")
    ns = ap.parse_args(argv)
    cfg = _load_cfg(ns.config)

    if ns.cmd == "dispatch":
        cmd_dispatch(cfg, ns.issue)
        return 0
    if ns.cmd == "check":
        runs_raw = _gh(
            "run", "list", "--workflow", ns.workflow, "--limit", "30", "--json", "createdAt,conclusion,status,displayTitle"
        )
        runs = [
            {
                "created_at": r["createdAt"],
                "conclusion": r.get("conclusion"),
                "status": r["status"],
                "hard": "[hard]" in (r.get("displayTitle") or ""),
            }
            for r in runs_raw
        ]
        prs = _gh("pr", "list", "--state", "open", "--limit", "100", "--json", "headRefName")
        n_agent = sum(1 for p_ in prs if p_["headRefName"].startswith("agent/"))
        d = decide(cfg, runs, n_agent, role=ns.role, hard=ns.hard)
        _emit(d)
        return 0
    issues = _gh(
        "issue", "list", "--label", "agent-ready", "--state", "open", "--limit", "100", "--json", "number,labels,createdAt"
    )
    prs = _gh("pr", "list", "--state", "open", "--limit", "100", "--json", "headRefName")
    for n in pick_issues(issues, [p_["headRefName"] for p_ in prs], ns.limit):
        print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
