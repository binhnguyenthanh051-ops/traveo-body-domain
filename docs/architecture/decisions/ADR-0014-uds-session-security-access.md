# ADR-0014: UDS session and security-access design (M3)

**Status:** accepted · **Date:** 2026-07-02

## Context

The session layer (ADR-0012 D1) owns two independent state machines — session type and
security access — plus SID dispatch and UDS-level timing. M3 needs programming-session entry
and a security-access gate (seed/key) before the download services are reachable; the
*mechanism* is the point for M3, not cryptographic strength (M4/M5 territory).

## Decisions

### D1. Session-type state machine (State pattern)

```
DEFAULT ──0x10 progSession──▶ PROGRAMMING ──S3 timeout / 0x10 default──▶ DEFAULT
```

Only `PROGRAMMING` session exposes the download/routine/reset services; `DEFAULT` exposes
0x10 and nothing else in M3 scope (data/DTC services are a later, App-side reuse of this same
server, not FBL scope). Dropping back to `DEFAULT` on `S3_server` timeout (ADR-0013 D3)
also re-locks security access (D2) — a stale unlock must not survive a session timeout.

### D2. Security-access state machine (State pattern)

```
LOCKED ──0x27 requestSeed──▶ SEED_SENT ──0x27 sendKey (correct)──▶ UNLOCKED
                                  │
                                  └──0x27 sendKey (wrong)──▶ LOCKED
```

`UNLOCKED` gates `requestDownload`/`transferData`/`requestTransferExit`/`routineControl`
(erase+verify)/`ECUReset` per ISO 14229's securityAccess model. A session drop (D1) or an
`S3` timeout re-locks unconditionally.

### D3. Seed/key algorithm — deliberately simple, honestly so

M3's algorithm is a fixed, non-cryptographic transform (seed = a counter/LFSR value, key =
seed combined with a fixed constant by a simple reversible operation) — it demonstrates the
*state machine and gating*, not real security. This carries the same honesty note as
ADR-0008 D3's CRC32-≠-authenticity point: **a simple seed/key proves the mechanism works; it
is not a security boundary.** Upgrading the algorithm's strength is not a committed milestone
item (M4/M5 focus on image signature and SecOC, respectively) — flagged here so it is not
mistaken for a real access-control guarantee.

### D4. Timing

P2/P2\*/S3 values are set in ADR-0013 D3 and enforced here (session layer, not transport).
`P2*` (extended response, 0x78 "response pending") is used only around the erase/program
operations that can legitimately exceed `P2` — not a general-purpose slow-path escape.

## Host/target split (ADR-0001)

| Host-testable core | Target-only |
|---|---|
| Both state machines (D1, D2) | — |
| Seed/key transform (D3) | — |
| P2/P2\*/S3 timer logic | `fbl_port_now_ms` (existing) |

Everything here is pure logic over a fake clock — no new port surface.

## Consequences

- (+) Both state machines are independent and fully host-testable, including the
  interrupted-session case (S3 timeout mid-download re-locks and drops session, but does not
  touch flash — D1/D2 are session-layer concerns, download bookkeeping is handler-layer,
  ADR-0012 D4).
- (−) The seed/key algorithm provides no real security in M3 — accepted and documented, not
  hidden (same posture as ADR-0008 D3's CRC32 note).

## Alternatives considered

- Real crypto-backed seed/key now — rejected for M3; out of scope, and the mechanism (state
  machine + gating) is the thing being demonstrated, not the primitive.
- A single combined session+security state machine — rejected; they have different
  triggers/lifetimes (security can re-lock without a session change on a bad key attempt) and
  conflating them would make the S3/session-drop re-lock rule harder to reason about.
