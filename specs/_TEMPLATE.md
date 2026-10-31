---
id: short-kebab-name              # unique slug; becomes label spec:<id>
goal: One or two sentences. What is true when this is done?
acceptance:                       # commands that exit 0 only when the work is done (machine-checkable)
  - uv run pytest tests/test_thing.py
scope:                            # path globs the agent may touch (never .github/.autopilot/.claude/AGENTS.md)
  - src/thing/**
  - tests/**
non_goals:                        # what NOT to do (reduces scope creep)
  - no new dependencies
risk_tier: 2                      # 1 docs/tests/deps | 2 feature | 3 auth, data, infra (never auto-merged)
max_runs: 3                       # agent attempts before it stops and asks you
autonomy: inherit                 # inherit | standard | yolo (yolo only if repo mode is yolo)
# hard: true                      # opus/high; tier-A repos only, max 1 per day
---

Context, links, design notes. Anything the agent needs that is not obvious from the code.

Run `ap handshake specs/<this-file>.md` to see whether the system accepts it and what it will do.
