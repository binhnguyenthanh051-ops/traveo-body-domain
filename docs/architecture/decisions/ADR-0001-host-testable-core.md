# ADR-0001: Keep hardware-independent logic host-testable

**Status:** accepted · **Date:** EP.03

## Context
Embedded code is often impossible to test without the target board, which kills CI and
slows iteration. But much of this system's logic (message packing, scheduler bookkeeping,
EEPROM sector management, message authentication) is pure logic that doesn't *need* the chip.

## Decision
Hardware-independent logic lives in standalone modules that compile with plain GCC on an
x86 host and are unit-tested there. All chip access goes through interfaces in
`common/hal/include`; logic modules never include vendor/ModusToolbox headers. Target-only
pieces (context-switch asm, flash drivers, key storage) sit behind those interfaces, with
host "fake" implementations for tests.

## Consequences
- (+) CI can build and test on every push without hardware.
- (+) Forces clean seams between policy (logic) and mechanism (hardware).
- (−) Some indirection; a few things need a host fake before they can be tested.
- (−) On-target behaviour (timing, real flash) still needs board testing — host tests are
  necessary, not sufficient.

## Alternatives considered
- On-target testing only (HIL): higher fidelity, but slow and gates every push on hardware.
  Deferred as a future extension, not the day-one default.
