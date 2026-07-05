# ADR-0002: CAN ID allocation scheme

**Status:** accepted · **Date:** EP.05

## Context
Even a 2-node toy network needs a deliberate ID scheme, or it rots into magic numbers.
Lower CAN IDs win arbitration (higher priority).

## Decision
Allocate by direction and priority band:
- `0x100–0x1FF`: Gateway → Actuator commands (control traffic, higher priority).
- `0x200–0x2FF`: Actuator → Gateway reports (telemetry, lower priority).
Central definitions live in `common/messages/include/body_msgs.h` — no node defines its
own IDs.

## Consequences
- (+) Priority is visible from the ID; one place to reason about bus arbitration.
- (+) Both nodes share one source of truth.
- (−) Flat scheme won't scale to a real multi-ECU network — revisited in the EP.21 scaling essay.

## M3 extension — diagnostic ID band

*Added 2026-07-02 for M3 (UDS reprogramming, ADR-0012/0013). The original scheme had no
diagnostic request/response pair — control/telemetry traffic and diagnostic traffic must not
share a band.*

- `0x700–0x7FF`: reserved **Diagnostics** band, physical addressing only for M3 (functional/
  broadcast addressing deferred — the band leaves room for it later without renumbering).
- `0x7A0`: Node A diagnostic request (tester → Node A).
- `0x7A8`: Node A diagnostic response (Node A → tester).

Central definitions still live alongside the existing IDs — no node/image defines its own.
