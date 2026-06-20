# ADR-0003: Adopt MISRA C:2012 as the coding standard

**Status:** accepted · **Date:** EP.03

## Context
This portfolio targets an automotive embedded architect role. In that domain, MISRA C is
the assumed baseline for production C. Coding to it (and being able to talk about its
rules and deviation process) is part of what the portfolio needs to demonstrate.

## Decision
Write all target/production C to MISRA C:2012. Enforce automatable rules via cppcheck's
MISRA addon in CI; uphold the rest via review and via coding rules in `CLAUDE.md`. Host
test harnesses are explicitly out of scope. Maintain a documented deviation log in
`docs/coding-standard.md`.

## Consequences
- (+) Signals production discipline; gives concrete material for blog posts (the deviation
  process is genuinely interesting and rarely written about well).
- (+) Forces the hardware/logic seam to stay clean — MISRA and the host-testability bet
  (ADR-0001) pull in the same direction.
- (−) Some friction: MISRA flags patterns that are fine in app code; each needs a
  documented deviation rather than a silent ignore.
- (−) Full automated checking needs the (licensed) MISRA rule-text file for the cppcheck
  addon; until then CI runs general static analysis, which overlaps but isn't identical.

## Alternatives considered
- No formal standard (just "clean code"): cheaper, but throws away a domain-expected signal.
- A commercial analyzer (Polyspace, PC-lint): higher fidelity, but cost/licensing makes it
  a poor fit for an open portfolio repo. cppcheck is the pragmatic open-source choice.
