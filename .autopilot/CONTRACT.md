# Autopilot contract (the handshake)

You drop a spec in `specs/*.md`. Run `ap handshake specs/<file>.md`. The system answers:

- **ACCEPTED**: it will queue issues, tells you the model, turn cap, run count and merge policy.
- **NEEDS-WORK**: lists exactly what is missing (usually machine-checkable acceptance) and how to fix it.
- **REJECTED**: the spec asks for something the Constitution forbids (contacting people, publishing,
  secrets, spend, protected paths). Do that part yourself.

## Spec fields
| field | required | meaning |
|---|---|---|
| id | yes | kebab-case slug, 3-49 chars |
| goal | yes | what is true when done (>= 20 chars) |
| acceptance | yes | list of commands that exit 0 when done (pytest, make, npm test, cargo, ...) |
| scope | yes | path globs the agent may touch; no `**`; never protected paths |
| non_goals | no (recommended) | what not to do |
| risk_tier | no (2) | 1 auto-merge on green; 2 skim (auto-merge only in yolo); 3 PR only, you review |
| max_runs | no (3) | 1-10 agent attempts |
| autonomy | no (inherit) | inherit / standard / yolo (yolo only if repo mode is yolo) |
| hard | no | opus/high, tier-A repos only, 1 per day |

## Labels on issues (phone-friendly commands)
`agent-ready` start | `hard` opus/high | `hold` pause this issue | `needs-human` agent stuck | `p0`/`p1` priority.
Comment `@claude` on an issue (as the repo owner) to dispatch it. Repo variables `AUTOPILOT_ENABLED` and
`AUTOPILOT_PAUSED` are the repo kill switches.
