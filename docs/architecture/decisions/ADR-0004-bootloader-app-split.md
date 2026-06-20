# ADR-0004: Bootloader + application split in one mono-repo

**Status:** accepted · **Date:** architecture phase

## Context
Node A needs field-reprogramming and secure boot — both of which imply a flash bootloader
(FBL) separate from the application. Real automotive ECUs split these. The FBL and app share
substantial driver code (CAN, UDS, flash/EEPROM, crypto).

## Decision
One mono-repo. Driver/logic modules live under `shared/` and are compiled into each image
with a **per-variant config** (e.g. CAN polled+minimal in the FBL, interrupt-driven+full in
the app; UDS exposes programming services in the FBL, data/DTC services in the app). Two
build variants per node where needed: `bootloader/` and `app/`, each with its own
`config/`, `src/`, and `linker/`. Node A has both; Node B is app-only.

## Consequences
- (+) Mirrors production structure; demonstrates reuse + configurability + build-variant
  thinking — core architect signals.
- (+) Shared code is written once, tested once (host CI), configured twice.
- (−) Requires a disciplined config strategy to avoid `#ifdef` sprawl — config data over
  conditional compilation where possible.
- (−) Two linker scripts + a memory map to maintain (also a feature: see overview §6).

## Alternatives considered
- Separate repos for FBL and app: cleaner isolation, but duplicates drivers and hides the
  reuse story. Rejected — the shared-with-variants design is the more senior demonstration.
- Single image (no FBL): simpler, but throws away the reprogramming + secure-boot content
  that is central to the portfolio.
