## What

<!-- One or two sentences: what this PR changes. -->

## Why

<!-- The reasoning / the decision behind it. Link the milestone, ADR, or review finding:
     e.g. "Implements M1 handshake (overview §3)", "Fixes F1 from ADR-0009 review".
     This is the part that becomes blog material — say the *why*, not just the *what*. -->

## How verified

<!-- Tick what applies; delete the rest. -->
- [ ] `make test` green (host unit tests)
- [ ] `make lint` green (cppcheck / MISRA-oriented)
- [ ] On-target tested (board): <!-- what you observed -->
- [ ] Bench-testable (CLAUDE.md rule 7): <!-- which contract event (or bus ID / structural /
      debugger reason) proves it, what triggers it (reset, a CAN frame, an attack script), and
      what host_tools/bench_summary.py shows. New REQ rows carry an "Observed by" cell; a new
      bring-up aid has a row in docs/hardware/kits.md "Bring-up switches". -->
- [ ] N/A — docs only

## Notes / tradeoffs

<!-- Optional: alternatives considered, follow-ups, anything a reviewer (or future you,
     or an interviewer reading the history) should know. -->

## Checklist
- [ ] Branch + commits follow `GitRules.md` (Conventional Commits, sensible scope)
- [ ] No secrets/keys/signing material committed
- [ ] ADR added/updated if this involved a non-trivial design decision
- [ ] Docs updated if behaviour or structure changed
