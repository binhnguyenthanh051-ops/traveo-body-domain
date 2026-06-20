# ADR-0005: FreeRTOS for the application; custom scheduler as a standalone deep-dive

**Status:** accepted · **Date:** architecture phase

## Context
The portfolio wants both a credible production system *and* a from-scratch scheduler as a
depth demonstration. Putting a hand-written scheduler under the product would make the whole
system depend on unproven kernel code.

## Decision
The application runs on **FreeRTOS** (proven, free, supported on TRAVEO™ T2G). The
**custom preemptive scheduler** lives in `scheduler/` as a **standalone educational module**,
not linked into any product image. It also anchors the EP.14 comparison (representative task
set on the custom scheduler vs FreeRTOS).

## Consequences
- (+) Product depends on a vetted kernel; risk where it belongs.
- (+) The scheduler can be ambitious/experimental without endangering the system.
- (+) Keeps the "what FreeRTOS hides" narrative and gives a concrete A/B comparison.
- (−) The scheduler isn't "used in production" in this project — framed honestly as a
  deep-dive, which is the accurate and defensible claim.

## Alternatives considered
- Custom scheduler as the app RTOS: maximal "look what I built", but couples product
  correctness to unproven code and is a weaker risk-management signal. Rejected.
- RTA-OS / other commercial AUTOSAR OS: realistic in industry, but licensing makes it unfit
  for an open portfolio. Referenced for breadth, not used.
