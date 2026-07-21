# Brief — `checks/` pipeline: first two deterministic checkers

> **Role for Claude Code:** these are **deterministic scripts, not AI agents.** No LLM calls.
> Each check has an exact right answer, exits non-zero on violation, runs locally and in CI, and
> is fast enough to gate a merge. Judgment stays with me; these only catch what I *forgot*.
> Depends on `traceability-convention.md` (the tag scheme + `checks/rules.yml`).

## Why scripts, not agents (hold this line)
An LLM that's right 95% of the time cannot gate a merge; a `grep` that's right 100% can. Anything
needing reading-comprehension/judgment is explicitly OUT of this layer — it belongs to the
separate, advisory, non-gating LLM reviewer built later (and only if manual review is the
bottleneck). Do not let these checkers "get smart."

## Language / placement
- `checks/` directory at repo root. Python 3.11 (matches `host_tools/`) or POSIX shell — pick one,
  keep it dependency-light. Prefer stdlib; if YAML rules, allow a JSON fallback to avoid a dep.
- Each checker: standalone, `checks/<name>.py`, `--root .` arg, prints violations, exit 0/1.
- A `make check` target runs all of them; wire into `.github/workflows/ci.yml` as a gating job.
- Unit-test the checkers themselves (they're logic too) with a couple of fixture dirs
  (a passing sample, a violating sample) — dogfooding the host-testable discipline.

## Checker 1 — layer-bleed / host-testability (`checks/layers.py`)
**What:** enforce the forbidden-dependency rules in `checks/rules.yml` (§4–5 of the convention).
- For `host_testable`: fail if any non-excluded file under the configured paths `#include`s a
  forbidden vendor header pattern or references a forbidden symbol (FreeRTOS etc.).
- For each `layers[]` entry: fail if a file under that layer's paths references a forbidden symbol
  (regex/glob against the source text).
**Input:** `rules.yml` (data-driven — adding a rule never edits the script).
**Output:** per violation — file:line, the rule name, the offending token. Exit 1 if any.
**Watch-fors:** don't flag matches inside comments/strings if easily avoidable (a simple
strip-line-comments pass is enough — don't over-engineer a C parser); allow an inline
`/* @allow <rule> : reason */` escape hatch for deliberate, documented exceptions (and print those
as a summary so exceptions stay visible).

## Checker 2 — ADR↔test traceability (`checks/traceability.py`)
**What:** parse IDs and tags (§1–2 of the convention) and enforce the links.
- Discover all `ADR-NNNN` (with status) from `docs/architecture/decisions/`, all `REQ-*` from
  `docs/requirements/`, and all `@impl`/`@test <ID>` tags across the source tree.
- **Fail if:** an `accepted` ADR (not `@design-only`) has zero `@test` references; a `REQ-*` has no
  `@impl` or no `@test`; any `@impl`/`@test` references an ID that doesn't exist (dangling tag).
- **Emit** `docs/traceability.md` — a generated matrix (`ID → impl files → test files → status`).
**Output:** the list of gaps/dangling tags (exit 1), plus the regenerated matrix.
**Watch-fors:** ADR status must be parseable (standardize the `**Status:** accepted` line — you
already use it); treat `superseded`/`proposed` ADRs as not-required; make `Dn` granularity a
config flag (start lax: ADR-level; tighten to sub-decision later if useful).

## Deliverables (in order)
1. Confirm/adjust the tag scheme + `rules.yml` shape from `traceability-convention.md` (quick).
2. `checks/layers.py` + `checks/traceability.py`, each with a passing + violating fixture and a
   tiny test.
3. `make check` target; a gating `checks` job in CI; add both to branch-protection required checks.
4. Backfill tags on M1–M4 opportunistically; apply from M5 onward by default.

## Explicitly NOT now (scope discipline)
- No LLM reviewer, no adversarial-design agent, no "does code match ADR intent" semantic check —
  those are the advisory layer, later, non-gating.
- No general "AI agent framework." This is a handful of boring scripts that keep paying every commit.
- MISRA/cppcheck and unit tests already exist — just ensure `make check` runs alongside them.

## Portfolio note
"I enforce my own architecture mechanically — layer isolation and decision-to-test traceability
gate every merge" is a real architect artifact (and maps to the Bosch role's architecture/design-
review emphasis). Worth a short `docs/` writeup and a blog beat once it's running.
