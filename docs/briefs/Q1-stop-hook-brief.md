# Brief Q1 — Stop hook: the host quality gate

> **How to use this:** paste as your opening message to Claude Code in the repo root.
> It has `CLAUDE.md`; this brief narrows the task. Design first — no code until I agree.

## Goal

Claude must not be able to finish a turn while the host gate is red. Add a **Stop hook**
(committed in `.claude/settings.json`) that runs the host checks. If they fail, the hook
blocks the stop and feeds the failure back to Claude, so Claude fixes it before handing back.

The gate: `make test && make lint`. This is host only: no board, no ModusToolbox firmware
build (too slow for every turn; it stays a manual step).

## Hard constraints

1. **Only when it matters.** Run the gate only if `.c`/`.h`/`Makefile`/test files changed in
   the working tree. A docs-only turn must stop instantly.
2. **No infinite loop.** Use the Stop hook input (`stop_hook_active`, see the hooks docs) or
   a retry cap. After the cap, let the stop happen and say plainly that the gate is still red.
3. **Never block on missing tools.** If `make`, `gcc` or the lint tool is missing, print a
   clear warning and exit 0. The same settings file will also load in cloud sessions (the
   noon reviewer, brief Q2), where the toolchain may differ.
4. **Runs on my Windows machine** (the shell Claude Code uses there) **and on Linux.**
   Put the logic in a script (e.g. `tools/hooks/stop_gate.sh`), not inline JSON.
5. **Short failure output.** Pass Claude the failing test names and the first lint errors,
   not 500 lines of log.
6. **Comprehension gate (docs/workflow.md):** explain the hook lifecycle and exit codes
   so I can explain them in an interview.

## Propose first

- How the hook detects "source changed" (git diff vs HEAD? include untracked files?).
- How long `make test` and `make lint` take today. If the total is over ~60 s, propose how
  to keep the gate fast.
- Exit-code and stderr behaviour for block vs allow, and the loop guard.
- One alternative, e.g. a PostToolUse hook after each edit instead of Stop, and why not.

## Deliverables (in order)

1. The design discussion above. **Wait for my agreement.**
2. `tools/hooks/stop_gate.sh` + the `Stop` entry in `.claude/settings.json`.
3. **Demo:** break one Unity test on purpose, show the hook blocking and Claude fixing it,
   then revert. Paste the evidence (the hook output) in the session.
4. One short section in `CLAUDE.md`: "The Stop hook runs the host gate; firmware builds
   are manual." Keep it to 3 lines.
5. Commit + push.

Time box: ~1 h. Low-energy option: steps 1–2 only, demo next session.
