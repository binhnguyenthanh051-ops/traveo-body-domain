# Coding standard

## Standard: MISRA C:2012 (with Amendment 1/2 guidance)

Target/production C in this project is written to **MISRA C:2012**. This is the
embedded-automotive default and is the discipline the portfolio is meant to demonstrate.

> Honesty note: this is a *learning/portfolio* project. "Written to MISRA" here means the
> rules are the standard we code to and check automatically where we can — not that the
> project carries a formal, audited compliance certificate. Deviations are documented
> rather than hidden (that documentation *is* the MISRA-compliant way to deviate).

## Scope

| Code | MISRA applies? |
|------|----------------|
| Target firmware + hardware-independent logic (`scheduler`, `eeprom_emu`, `common/messages`, `security`, `common/hal`, node apps) | **Yes** |
| Host test harnesses (`*/tests/`) | No — test code may use `printf`, function-like macros, etc. Treated as off-target tooling. |
| Python host tools (`host_tools/`) | N/A (not C) |

Keeping production logic MISRA-clean while letting test harnesses use `printf`/macros is a
standard and defensible split — it's worth a sentence in the EP.03/EP.09 posts.

## Enforcement

- Automatable rules are checked with **cppcheck**'s MISRA addon in CI (`make lint`).
  cppcheck is free; the MISRA addon needs a rule-texts file you supply locally (the rule
  *texts* are copyrighted by MISRA, the addon is not). Until that file is in place, CI runs
  cppcheck's general static analysis, which already catches a large overlapping set.
- Not every MISRA rule is statically decidable; the rest are upheld by review and by the
  rules in `CLAUDE.md` so Claude Code applies them while generating code.

## Accepted deviations (living list)

Record each deviation here with rule, location, and rationale. Starting set:

| Rule | Where | Rationale |
|------|-------|-----------|
| 15.5 (single point of exit — *advisory*) | guard-clause functions in `common/messages`, etc. | Early-return on invalid input is clearer and safer than a single-exit flag dance. Advisory rule, deliberately deviated. |
| 21.6 (no `<stdio.h>`) | `*/tests/` only | Host test harnesses use `printf`. Production code never includes `<stdio.h>`. |

Add rows as real deviations come up — an honest, growing deviation log is itself a strong
architect signal.
