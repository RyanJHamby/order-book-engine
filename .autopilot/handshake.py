#!/usr/bin/env python3
# ruff: noqa
# (kit-managed file: host-repo lint rules do not apply)
"""Autopilot handshake: lint a spec against the contract and say what the system will do.

Copied into each repo as .autopilot/handshake.py (stdlib + PyYAML only, zero tokens).
Verdicts: ACCEPTED (exit 0), NEEDS-WORK (exit 1), REJECTED (exit 2).
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from dataclasses import dataclass, field

PROTECTED = [".github/**", ".autopilot/**", ".claude/**", "AGENTS.md", "CLAUDE.md", ".mcp.json", "CODEOWNERS"]
RUNNERS = {
    "pytest",
    "uv",
    "uvx",
    "python",
    "python3",
    "pip",
    "ruff",
    "mypy",
    "pyright",
    "npm",
    "pnpm",
    "yarn",
    "npx",
    "node",
    "tsc",
    "cargo",
    "rustc",
    "go",
    "make",
    "cmake",
    "ctest",
    "bash",
    "sh",
    "flutter",
    "dart",
    "swift",
    "xcodebuild",
    "mvn",
    "gradle",
    "./gradlew",
    "bun",
    "deno",
    "ruby",
    "bundle",
    "rake",
    "clang-tidy",
    "pre-commit",
    "just",
}
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{2,48}$")

# Spec text that asks for something the Constitution forbids -> REJECTED.
FORBIDDEN = [
    (
        r"\b(send|draft and send|reply to|forward|write)\b[^.\n]{0,40}\b(email|e-mail|gmail|slack|discord|dm|text message|sms)\b",
        "sends messages to people (Constitution 1)",
    ),
    (
        r"\b(post|publish|tweet|share)\b[^.\n]{0,40}\b(linkedin|substack|twitter|x\.com|blog|medium|newsletter|social)\b",
        "publishes to social/publishing platforms (Constitution 1)",
    ),
    (
        r"\b(calendar|inbox|todoist|asana|schedule a meeting|book a)\b[^.\n]{0,40}\b(edit|update|create|add|read|check|clean|triage|modify)\b|\b(edit|update|create|add|read|check|clean|triage|modify)\b[^.\n]{0,40}\b(calendar|inbox|todoist|asana)\b",
        "touches calendars/inboxes/task managers (Constitution 1)",
    ),
    (
        r"\b(contact|reach out to|message|notify|cold[- ]?email|apply to|dm)\b[^.\n]{0,40}\b(recruiter|hiring manager|maintainer|customer|employer|people|users|candidate)s?\b",
        "contacts people (Constitution 1)",
    ),
    (
        r"\b(comment on|open (an )?issue (in|on)|open (a )?pr (to|against)|submit (a )?pr)\b[^.\n]{0,40}\b(upstream|third[- ]party|someone else|external)\b",
        "acts on repos the owner does not own (Constitution 1)",
    ),
    (
        r"\b(publish|release|upload)\b[^.\n]{0,40}\b(pypi|npm|crates\.io|app store|play store|docker hub|package)\b|\bnpm publish\b|\btwine upload\b",
        "publishes artifacts (Constitution 1)",
    ),
    (
        r"\b(read|print|cat|echo|dump|commit|exfiltrate|upload)\b[^.\n]{0,40}(\.env|secrets?|api keys?|credentials|private keys?|tokens?)\b",
        "touches secrets (Constitution 2)",
    ),
    (
        r"\b(scrape|crawl)\b[^.\n]{0,60}\b(linkedin|instagram|facebook|login|paywall|behind auth)\b|\b(bypass|circumvent|evade)\b[^.\n]{0,30}\b(captcha|paywall|rate limit|tos|terms of service|auth)\b|\b(malware|ransomware|keylogger|ddos|credential stuffing|exploit .* without)\b",
        "illegal or ToS-violating work (Constitution 3)",
    ),
    (
        r"\b(deploy|ship|push)\b[^.\n]{0,30}\b(to )?(prod|production)\b|\bterraform apply\b|\b(provision|spin up|rent)\b[^.\n]{0,30}\b(gpu|instance|cluster|ec2|cloud)\b",
        "spends money / deploys (Constitution 4)",
    ),
    (
        r"\bforce[- ]push\b|\bpush (directly )?to (main|master)\b|\bdisable (commit )?signing\b|\bskip (hooks|ci|tests)\b|\bsquash[- ](merge|commits?|history)\b",
        "violates git discipline (Constitution 5)",
    ),
]
_FORBIDDEN_RES = [(re.compile(p, re.I), msg) for p, msg in FORBIDDEN]


@dataclass
class Verdict:
    status: str  # ACCEPTED | NEEDS-WORK | REJECTED
    reasons: list[str] = field(default_factory=list)
    fixes: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    plan: list[str] = field(default_factory=list)
    spec: dict = field(default_factory=dict)

    @property
    def exit_code(self) -> int:
        return {"ACCEPTED": 0, "NEEDS-WORK": 1, "REJECTED": 2}[self.status]

    def render(self) -> str:
        lines = [f"HANDSHAKE: {self.status}"]
        for title, items in (
            ("Why", self.reasons),
            ("To fix", self.fixes),
            ("Notes", self.warnings),
            ("What the system will do", self.plan),
        ):
            if items:
                lines.append(f"\n{title}:")
                lines += [f"  - {i}" for i in items]
        return "\n".join(lines)


def parse_spec(text: str) -> tuple[dict, str]:
    m = re.match(r"^---\s*\n(.*?)\n---\s*\n?(.*)$", text, re.S)
    if not m:
        return {}, text
    import yaml  # type: ignore

    try:
        data = yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError as e:
        raise ValueError(f"frontmatter is not valid YAML: {e}") from e
    if not isinstance(data, dict):
        raise ValueError("frontmatter must be a YAML mapping")
    return data, m.group(2)


def _matches(path: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if pat.endswith("/**") and (path == pat[:-3] or path.startswith(pat[:-2])):
            return True
        if fnmatch.fnmatchcase(path, pat):
            return True
    return False


def _looks_like_command(cmd: str) -> bool:
    first = cmd.strip().split(" ", 1)[0]
    return first in RUNNERS or first.startswith(("./", "scripts/"))


def evaluate(text: str, repo_cfg: dict | None = None) -> Verdict:
    repo_cfg = repo_cfg or {}
    repo_mode = repo_cfg.get("mode", "standard")
    tier = repo_cfg.get("tier", "B")
    try:
        meta, body = parse_spec(text)
    except ValueError as e:
        return Verdict("NEEDS-WORK", [str(e)], ["Fix the YAML frontmatter between the --- lines."])
    v = Verdict("ACCEPTED", spec=meta)

    # --- REJECTED: Constitution violations (scan whole text, ignore the non_goals section) ---
    scan = re.sub(r"(?im)^\s*non_goals:.*?(?=^\S|\Z)", "", text, flags=re.S)
    scan = re.sub(r"(?im)^#+\s*non[- ]goals.*?(?=^#|\Z)", "", scan, flags=re.S)
    for rx, msg in _FORBIDDEN_RES:
        if rx.search(scan):
            v.reasons.append(f"Spec asks for work that {msg}.")
    scope = meta.get("scope") or []
    if isinstance(scope, str):
        scope = [scope]
    hit = [
        s
        for s in scope
        if _matches(str(s).lstrip("./"), PROTECTED) or str(s).startswith(tuple(p[:-2] for p in PROTECTED if p.endswith("/**")))
    ]
    if hit:
        v.reasons.append(f"Scope includes protected paths {hit}; agents may never edit these.")
    if v.reasons:
        v.status = "REJECTED"
        v.fixes.append("Remove those parts from the spec and handle them yourself; autopilot only ships code in your repos.")
        return v

    # --- NEEDS-WORK: contract gaps ---
    gaps: list[str] = []
    fixes: list[str] = []
    sid = meta.get("id")
    if not isinstance(sid, str) or not SLUG_RE.match(sid):
        gaps.append("`id` missing or not a kebab-case slug (3-49 chars).")
        fixes.append("Add `id: short-kebab-name`.")
    goal = meta.get("goal")
    if not isinstance(goal, str) or len(goal.strip()) < 20:
        gaps.append("`goal` missing or too short to be unambiguous.")
        fixes.append("Add `goal:` with one or two sentences saying what is true when this is done.")
    acc = meta.get("acceptance")
    if not isinstance(acc, list) or not acc:
        gaps.append("`acceptance` must be a non-empty list of commands that exit 0 when the work is done.")
        fixes.append(
            'Add e.g. `acceptance: ["uv run pytest tests/test_x.py"]`. If it cannot be machine-checked, do it yourself or add a test first.'
        )
    else:
        for i, c in enumerate(acc):
            if not isinstance(c, str) or not _looks_like_command(c):
                gaps.append(f"acceptance[{i}] ({c!r}) is not a runnable command.")
                fixes.append("Express it as a command (pytest, make, npm test, ...) or write the test as part of the spec.")
    if not scope:
        gaps.append("`scope` (path globs the agent may touch) is missing.")
        fixes.append('Add `scope: ["src/foo/**", "tests/**"]`.')
    else:
        broad = [s for s in scope if str(s).strip() in {"*", "**", "**/*", ".", "./", "/"}]
        if broad:
            gaps.append(f"`scope` is too broad: {broad}.")
            fixes.append("Narrow scope to the directories this spec actually needs.")
    rt = meta.get("risk_tier", 2)
    if rt not in (1, 2, 3):
        gaps.append("`risk_tier` must be 1, 2 or 3.")
        fixes.append("Use 1 (docs/tests/deps), 2 (feature), 3 (auth, data, infra).")
    mr = meta.get("max_runs", 3)
    if not isinstance(mr, int) or not 1 <= mr <= 10:
        gaps.append("`max_runs` must be an integer 1-10.")
        fixes.append("Set `max_runs: 3` (agent attempts before it stops and asks you).")
    aut = meta.get("autonomy", "inherit")
    if aut not in ("inherit", "standard", "yolo"):
        gaps.append("`autonomy` must be inherit|standard|yolo.")
        fixes.append("Use `autonomy: inherit`.")
    elif aut == "yolo" and repo_mode != "yolo":
        gaps.append(f"spec requests yolo but this repo is `{repo_mode}`.")
        fixes.append("Remove `autonomy: yolo` or set `mode: yolo` in .autopilot/repo.yaml yourself.")
    if gaps:
        v.status = "NEEDS-WORK"
        v.reasons, v.fixes = gaps, fixes
        return v

    # --- ACCEPTED: describe the deal ---
    if not meta.get("non_goals"):
        v.warnings.append("No `non_goals`; adding 1-2 reduces scope creep.")
    if not body.strip():
        v.warnings.append("Body is empty; add context, links, or design notes.")
    if len(scope) > 8:
        v.warnings.append("Large scope (>8 globs); consider splitting into multiple specs.")
    hard = meta.get("model") == "opus" or meta.get("hard") is True
    if hard and tier != "A":
        v.warnings.append("`hard` (opus) is reserved for tier-A repos; will run on sonnet instead.")
        hard = False
    caps = repo_cfg.get("max_turns", {}) if isinstance(repo_cfg.get("max_turns"), dict) else {}
    eff_mode = repo_mode if aut == "inherit" else aut
    turns = caps.get("hard" if hard else eff_mode, 60 if eff_mode == "yolo" else 25)
    v.plan = [
        f"Queue {1}+ issue(s) labeled `spec:{sid}`; `agent-ready` is set because acceptance is machine-checkable.",
        f"Model/effort: {'opus/high' if hard else 'sonnet/medium'}, up to {turns} turns per run, up to {mr} run(s).",
        "Open a PR on branch `agent/<issue>-*`; CI + guard must pass; commits are GitHub-verified; merge is a merge commit.",
        {
            1: "Risk tier 1: auto-merge on green (standard/yolo).",
            2: "Risk tier 2: auto-merge only in yolo mode; otherwise you skim and merge.",
            3: "Risk tier 3: PR only, CODEOWNERS review by you, never auto-merged.",
        }[rt],
        "Zero outbound contact: no email/Slack/calendar/social/publishing, per the Constitution.",
    ]
    return v


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("specs", nargs="+")
    ap.add_argument("--config", default=".autopilot/repo.yaml")
    ap.add_argument("--warn-only", action="store_true", help="exit 0 for NEEDS-WORK (still fails on REJECTED)")
    ns = ap.parse_args(argv)
    cfg: dict = {}
    try:
        import yaml  # type: ignore

        with open(ns.config) as fh:
            cfg = yaml.safe_load(fh) or {}
    except Exception:
        pass
    worst = 0
    for path in ns.specs:
        with open(path) as fh:
            v = evaluate(fh.read(), cfg)
        print(f"== {path}\n{v.render()}\n")
        code = v.exit_code
        if ns.warn_only and code == 1:
            code = 0
        worst = max(worst, code)
    return worst


if __name__ == "__main__":
    sys.exit(main())
