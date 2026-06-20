# CLAUDE.md — context for Claude Code

This file gives Claude Code the standing context for this repo. Keep it short, factual,
and current — update it when an architectural decision changes.

## What this project is

A portfolio project: a 2-node automotive body-domain network on Infineon TRAVEO™ T2G,
written to demonstrate architecture-level embedded thinking for a Medium series. Not
production software. Prefer clarity and explainability over cleverness — the code is
read by humans (and quoted in blog posts) as much as it is run.

## Hardware facts (do not guess these)

- Board: `CYTVII-B-E-1M-SK`, MCU `CYT2B7` (TRAVEO™ T2G Body Entry).
- Cores: 160 MHz Arm Cortex-M4F (primary) + 100 MHz Arm Cortex-M0+ (peripheral/security).
  This is **asymmetric** dual-core, NOT symmetric multicore.
- Memory: 1 MB code flash, 96 KB work flash, 128 KB SRAM.
- No data/instruction cache on these cores (cache discussion is design-essay only).
- Both cores have an MPU. IPC uses hardware semaphores.
- Toolchain: Infineon ModusToolbox for firmware. GCC for host-side tests.

## Architectural rules (enforce these in suggestions)

1. Hardware-independent logic (`scheduler`, `eeprom_emu`, `common/messages`, `security`
   logic) must NOT include vendor/ModusToolbox headers. It talks to hardware only through
   interfaces in `common/hal/include`.
2. Anything in those modules must compile and unit-test on the host with plain GCC.
3. Target-only code (context-switch assembly, flash drivers, key storage) lives in the
   node apps or behind a HAL interface with a host "fake" implementation for tests.
4. Every non-trivial design choice gets a short ADR in `docs/architecture/decisions`.
5. Target/production C is written to **MISRA C:2012** (see `docs/coding-standard.md`).
   When generating C, apply MISRA: fixed-width types only, no implicit conversions, no
   `<stdio.h>` in production code, explicit braces, no hidden side effects. If a clean
   solution must break an advisory rule (e.g. 15.5 single-exit), do it deliberately and
   note it as a deviation rather than contorting the code. Host test harnesses in `*/tests/`
   are exempt and may use `printf`/macros.

## How I want to use you (Claude Code)

- Scaffolding, glue code, test fixtures, Python host tools, README/ADR drafting.
- When proposing an architecture or interface, also list the tradeoffs and at least one
  alternative — I write blog chapters from those discussions, so the reasoning matters
  as much as the result.
- Challenge my decisions when you see a problem; don't just agree.
- When you write code, prefer a matching host test in the module's `tests/` folder.

## Conventions

- C17 for host-testable modules; keep them freestanding-friendly (no libc assumptions
  beyond what tests need). Production C follows MISRA C:2012 (see `docs/coding-standard.md`).
- Test naming: `test_<module>.c`, one runner per module, must return non-zero on failure.
- Python: 3.11+, scripts under `host_tools/`, tests runnable with `pytest`.
