#!/usr/bin/env python3
# ruff: noqa
# (kit-managed file: host-repo lint rules do not apply)
"""Autopilot CI guard: deterministic, zero-token checks on a PR diff.

Copied into each repo as .autopilot/guard.py. The workflow runs the BASE-branch
copy so an agent PR cannot weaken the guard that judges it.

Checks (all PRs): secrets, outbound-comms code/URLs, test-assertion deletions.
Extra checks when --agent: protected paths, .mcp.json, diff size cap, append-only paths (guard.append_only: eval fixtures).
Exit 0 = clean, 1 = violations.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass, field

DEFAULT_PROTECTED = [
    ".github/**",
    ".autopilot/**",
    ".claude/**",
    "AGENTS.md",
    "CLAUDE.md",
    ".mcp.json",
    "CODEOWNERS",
    ".github/CODEOWNERS",
]
DEFAULT_MAX_DIFF_LINES = 800
# Kit-managed files legitimately name messaging/publishing surfaces (regex tables, deny rules).
# Agents cannot edit these paths (protected), so exempting them from the comms check is safe.
COMMS_EXEMPT = [
    ".autopilot/**",
    ".claude/settings.json",
    ".claude/agents/**",
    ".github/kit-proposer-settings.json",  # the proposer's deny rules name publish/send commands to forbid them
    "AGENTS.md",
    "CLAUDE.md",
    ".github/workflows/agent-*.yml",
]

SECRET_PATTERNS = {
    "AWS access key": r"\bAKIA[0-9A-Z]{16}\b",
    "GitHub token": r"\b(ghp|gho|ghu|ghs|ghr|github_pat)_[A-Za-z0-9_]{20,}\b",
    "Anthropic key": r"\bsk-ant-[A-Za-z0-9_-]{16,}\b",
    "OpenAI-style key": r"\bsk-[A-Za-z0-9]{32,}\b",
    "Slack token": r"\bxox[abprs]-[A-Za-z0-9-]{10,}\b",
    "private key block": r"-----BEGIN (RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----",
    "Google API key": r"\bAIza[0-9A-Za-z_-]{35}\b",
    "generic secret assignment": r"""(?i)\b(api[_-]?key|secret|passwd|password|token)\b\s*[:=]\s*['"][A-Za-z0-9/+_\-]{24,}['"]""",
}

# Outbound-communication / social / admin surfaces. Matching added lines fail the guard.
COMMS_PATTERNS = {
    "SMTP/email library": r"\b(import|from)\s+(smtplib|yagmail|sendgrid|mailgun|postmarker|sib_api_v3_sdk)\b|require\(['\"](nodemailer|@sendgrid/mail|mailgun[.\w-]*)['\"]\)",
    "Slack/Discord/Teams": r"\bslack[_-]?sdk\b|slack_bolt|hooks\.slack\.com|discord\.com/api/webhooks|discord\.py|outlook\.office\.com/webhook",
    "SMS/voice": r"\btwilio\b|\bvonage\b|\bnexmo\b",
    "Social networks": r"\btweepy\b|api\.twitter\.com|api\.x\.com|linkedin\.com/(v2|rest)|api\.linkedin\.com|\bpraw\b|graph\.facebook\.com|substack\.com/api",
    "Google Workspace APIs": r"googleapiclient|gmail\.googleapis\.com|www\.googleapis\.com/(calendar|gmail)|calendar\.googleapis\.com|from google\.oauth2",
    "Task managers": r"api\.todoist\.com|app\.asana\.com/api",
    "Package publishing": r"\bnpm publish\b|\btwine upload\b|\buv publish\b|\bcargo publish\b",
}

_SECRET_RES = {k: re.compile(v) for k, v in SECRET_PATTERNS.items()}
_COMMS_RES = {k: re.compile(v) for k, v in COMMS_PATTERNS.items()}
_ASSERT_RE = re.compile(r"\b(assert\w*|expect\(|\.should|self\.assert\w+|def test_\w+|\bit\(['\"]|\btest\(['\"])")
_TEST_PATH_RE = re.compile(r"(^|/)(tests?|__tests__|spec)/|(^|/)test_[^/]*$|[._-](test|spec)\.\w+$")


@dataclass
class FileChange:
    path: str
    status: str = "M"  # A, M, D, R
    added: list[str] = field(default_factory=list)
    removed: list[str] = field(default_factory=list)


@dataclass
class Violation:
    check: str
    path: str
    detail: str

    def __str__(self) -> str:
        return f"[{self.check}] {self.path}: {self.detail}"


def _matches(path: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if pat.endswith("/**"):
            if path == pat[:-3] or path.startswith(pat[:-2]):
                return True
        if fnmatch.fnmatchcase(path, pat):
            return True
    return False


def _dep_names(ch: "FileChange") -> set[str]:
    import os, sys

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import charter

    return charter.added_dependencies(ch.path, ch.added, ch.removed)


def parse_diff(text: str) -> list[FileChange]:
    """Parse `git diff -U0 --no-color --no-renames` output."""
    changes: list[FileChange] = []
    cur: FileChange | None = None
    for line in text.splitlines():
        if line.startswith("diff --git "):
            cur = FileChange(path=line.split(" b/", 1)[-1])
            changes.append(cur)
        elif cur is None:
            continue
        elif line.startswith("new file mode"):
            cur.status = "A"
        elif line.startswith("deleted file mode"):
            cur.status = "D"
        elif line.startswith("+++ ") or line.startswith("--- "):
            continue
        elif line.startswith("+"):
            cur.added.append(line[1:])
        elif line.startswith("-"):
            cur.removed.append(line[1:])
    return changes


def analyze(
    changes: list[FileChange],
    *,
    agent: bool,
    protected: list[str] | None = None,
    max_diff_lines: int = DEFAULT_MAX_DIFF_LINES,
    comms_allow: list[str] | None = None,
    allow_new_dependencies: bool = True,
    append_only: list[str] | None = None,
) -> list[Violation]:
    protected = protected if protected is not None else DEFAULT_PROTECTED
    comms_allow = comms_allow or []
    out: list[Violation] = []
    total = 0
    removed_asserts = added_asserts = 0
    for ch in changes:
        total += len(ch.added) + len(ch.removed)
        if agent and _matches(ch.path, protected):
            out.append(Violation("protected-path", ch.path, "agents may not modify this path"))
        if agent and append_only and ch.status != "A" and _matches(ch.path, append_only):
            out.append(Violation("append-only", ch.path, "eval fixtures/cassettes may be added, never modified or deleted by an agent"))
        if agent and ch.path.endswith(".mcp.json"):
            out.append(Violation("mcp-config", ch.path, "MCP config is forbidden for agents"))
        for line in ch.added:
            for name, rx in _SECRET_RES.items():
                if rx.search(line):
                    out.append(Violation("secret", ch.path, f"possible {name} in added line"))
            if not _matches(ch.path, comms_allow + COMMS_EXEMPT):
                for name, rx in _COMMS_RES.items():
                    if rx.search(line):
                        out.append(Violation("comms", ch.path, f"{name}: outbound-comms surface added"))
        if agent and not allow_new_dependencies:
            for dep in sorted(_dep_names(ch)):
                out.append(Violation("new-dependency", ch.path, f"adds package {dep}; the repo charter forbids new dependencies"))
        if _TEST_PATH_RE.search(ch.path):
            removed_asserts += sum(1 for ln in ch.removed if _ASSERT_RE.search(ln))
            added_asserts += sum(1 for ln in ch.added if _ASSERT_RE.search(ln))
            if ch.status == "D":
                out.append(Violation("test-deletion", ch.path, "test file deleted"))
    if removed_asserts > added_asserts + 3:
        out.append(Violation("test-weakening", "(tests)", f"{removed_asserts} assertions/tests removed vs {added_asserts} added"))
    if agent and total > max_diff_lines:
        out.append(
            Violation("size", "(diff)", f"{total} changed lines exceeds cap {max_diff_lines}; split it or label needs-human")
        )
    return out


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def _load_cfg(path: str) -> dict:
    try:
        import yaml  # type: ignore

        with open(path) as fh:
            return yaml.safe_load(fh) or {}
    except Exception:
        return {}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True, help="base ref, e.g. origin/main")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--agent", default="false", help="true if the PR comes from an agent branch")
    ap.add_argument("--config", default=".autopilot/repo.yaml")
    ns = ap.parse_args(argv)
    cfg = _load_cfg(ns.config)
    guard_cfg = cfg.get("guard", {}) or {}
    diff = _git("diff", "-U0", "--no-color", "--no-renames", f"{ns.base}...{ns.head}")
    violations = analyze(
        parse_diff(diff),
        agent=str(ns.agent).lower() == "true",
        protected=DEFAULT_PROTECTED + list(guard_cfg.get("extra_protected", []) or []),
        max_diff_lines=int(guard_cfg.get("max_diff_lines", DEFAULT_MAX_DIFF_LINES)),
        comms_allow=list(guard_cfg.get("comms_allow", []) or []),
        append_only=list(guard_cfg.get("append_only", []) or []),
        allow_new_dependencies=bool(((cfg.get("charter") or {}).get("allow_new_dependencies", True))),
    )
    if not violations:
        print("autopilot guard: clean")
        return 0
    print("autopilot guard: FAILED")
    for v in violations:
        print(f"  - {v}")
        print(f"::error file={v.path}::{v.check}: {v.detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
