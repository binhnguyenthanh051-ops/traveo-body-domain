# ADR-0013: ISO-TP transport configuration (M3)

**Status:** accepted · **Date:** 2026-07-02

## Context

The transport layer (ADR-0012 D1) segments/reassembles multi-frame UDS messages over the
existing CANFD channel (ADR-0011). Scope for M3 is minimal-but-real: one tester, physical
addressing, poll-driven from the FBL super-loop (ADR-0012 D2).

## Decisions

### D1. Single concurrent session, static reassembly buffer

One tester, one physical address exchange — no session pool, no functional-addressing
fan-out. The reassembly buffer is sized to the worst-case `transferData` block
(`ISOTP_MAX_PAYLOAD`, default 4096 B, `#define`-configurable) and statically allocated — no
heap, matches MISRA/ADR-0003.

### D2. Poll integration

`isotp_poll(uint32_t now_ms)` is called once per FBL super-loop iteration. It drains
available CAN RX via the existing `can_hal_if_t`, advances the SF/FF/CF/FC state machine, and
sends flow-control frames synchronously from within the poll call when a multi-frame
reception needs a CTS. No callback, no queue — matches ADR-0012 D2's rejection of the App's
ISR/queue pattern for this image.

### D3. Timing (ISO 15765-2 / ISO 14229 defaults as the starting point)

| Parameter | Value | Owner |
|---|---|---|
| Block size (BS) | 8 | ISO-TP (flow control) |
| STmin | 0 ms | ISO-TP — CANFD's data-phase bandwidth (ADR-0011 D1) makes a minimum separation unnecessary |
| N_As / N_Bs / N_Cr | 1000 ms | ISO-TP frame timing |
| P2_server_max | 50 ms | UDS session (not transport — see below) |
| P2\*_server_max | 5000 ms | UDS session |
| S3_server | 5000 ms | UDS session |

**P2/P2\*/S3 live in the session layer, not here** — they are UDS-level timing, not ISO-TP
framing, and transport must not know about UDS (ADR-0012 D1). Full detail: ADR-0014.

### D4. New CAN ID band for diagnostics (extends ADR-0002)

The existing CAN ID scheme (0x100–0x1FF control, 0x200–0x2FF telemetry) has no diagnostic
request/response pair. See the ADR-0002 amendment for the reserved band and concrete IDs.
Physical addressing only for M3; functional addressing (broadcast diagnostic requests) is
deferred — nothing here precludes adding it later as a second accepted request ID.

### D5. The knock-window hook does not change shape

`fbl_port_tool_contact()` keeps its existing `bool` signature (ADR-0008 D2: "the core does
not change"). Its M3 target implementation checks for any inbound frame on the diagnostic
request ID — the knock window needs presence detection only; the real session/handler stack
(D2, ADR-0012) runs afterward, once the FBL has already committed to staying resident. An
earlier framing in this design session proposed changing this signature; that was wrong and
is corrected here rather than carried into the ADR.

## Host/target split (ADR-0001)

| Host-testable core | Target-only |
|---|---|
| SF/FF/CF/FC state machine, reassembly, timers | CANFD send/receive (`can_hal_if_t`, unchanged) |
| Buffer bookkeeping (offsets, expected length) | — |

## Consequences

- (+) No new CAN HAL surface — ISO-TP is pure logic over the existing interface.
- (+) The knock-window mechanism needed zero changes beyond its target implementation.
- (−) Single-session assumption means a second simultaneous tester is not supported — fine
  for a bench/field tool, revisit only if that becomes a real requirement.

## Alternatives considered

- Functional (broadcast) addressing alongside physical for M3 — deferred; adds an accept
  filter and multi-recipient bookkeeping with no M3 use case yet.
- STmin > 0 for pacing headroom — rejected for M3; CANFD's data-phase bandwidth
  (2 Mbit/s, ADR-0011 D1) makes it unnecessary, revisit if real hardware shows otherwise.

## To verify in the TRM / on silicon

- Achieved ISO-TP frame timing against N_As/N_Bs/N_Cr once the real CAN RX path is measured.
- Whether `ISOTP_MAX_PAYLOAD` = 4096 B comfortably fits the chosen flash program-row/erase
  chunking (ADR-0012 D6) once real values are confirmed.

## Review history

Design-reviewed in discussion (ADR-0012's companion), then corrected against silicon during
M3 Seam 2 bring-up (the FBL's first real ISO-TP round trip over the CAN bus, echoing a
message back through `node_a_gateway/bootloader/src/port_prog.c`'s bring-up loop). Finding
actioned:

- **S1** (silicon) — CAN FD's DLC only represents specific lengths (0–8, then
  12/16/20/24/32/48/64 — see ADR-0011 D3/D4's `dlc_to_len`/`len_to_dlc`). A frame whose real
  payload doesn't land exactly on one of those is transparently padded to the next valid
  length on the wire. `handle_ff()`/`handle_cf()` originally trusted the *received* frame's
  own reported length (post-padding) to size the copy, so trailing pad bytes were captured
  as message content — reproduced with a 162-byte round trip whose first-frame length
  (63 B) and one consecutive-frame length (42 B) both fell between valid DLC steps.
  Fixed: `ISOTP_FF_INITIAL_LEN` is now exactly 59 (5-byte header + 59 = 64, itself a valid
  length, so the first frame needs no padding at all), and `handle_cf()` derives how many
  bytes to copy from the ISO-TP *logical* state (bytes still needed against the first
  frame's declared total) rather than the physical frame length — correct regardless of
  chunk size, not just this one case. Regression-tested directly by
  `shared/diag/tests/test_isotp.c`'s `test_rx_cf_ignores_dlc_padding_beyond_logical_length`.
  The PC-side probe (`host_tools/isotp_bringup_probe/`) had the same bug in reverse (trusting
  the received frame length when reassembling the FBL's echo) and was fixed the same way.

**Note (added during M3 Seam 3 bring-up):** at the time S1 above was verified, `fbl_port_now_ms()`
was silently non-functional on this board (ADR-0008's Seam 3 finding S2 — `PRIMASK` left set,
so SysTick's interrupt was never taken). D3's `N_Cr` timeout therefore was not actually
exercised by Seam 2's success — a prompt, successful exchange never depends on the timeout
firing. See ADR-0008's review history for the fix; `N_Cr` now shares the same real clock the
knock dwell does.
