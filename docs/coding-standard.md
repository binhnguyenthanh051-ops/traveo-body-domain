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
| Dir 1.1 / Rule 1.2 (no reliance on unspecified/undefined behaviour; no language extensions) | `scheduler/src/sched.c` — `__builtin_ctz` in `sched_find_highest_ready_priority` | The priority-bitmap selector uses the `__builtin_ctz` compiler intrinsic (count-trailing-zeros → `RBIT+CLZ` on Cortex-M, `BSF/TZCNT` on x86). No portable C standard equivalent exists with the same single-instruction codegen. The argument is guarded against 0 at the call site (`ctz(0)` is undefined), and a portable binary-search fallback is documented in ADR-0009 §D3 if the extension must be removed. Encapsulated in one function. |
| Rule 4.3 (assembly encapsulated in dedicated functions) | `scheduler/src/sched_port_arm.c` (target port: PendSV/SysTick context switch) | The context switch requires inline/naked assembly (PSP manipulation, `{r4–r11}` + FP save/restore, EXC_RETURN). It is confined to the dedicated target-port translation unit, behind the `sched_port.h` interface; the host fake contains no assembly and the portable core never sees it. |

Add rows as real deviations come up — an honest, growing deviation log is itself a strong
architect signal.
