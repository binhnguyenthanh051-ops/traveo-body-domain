# ADR-0018: Cross-core IPC transport — shared-RAM mailbox + hardware semaphore (M4)

**Status:** accepted · **Date:** 2026-07-05

## Context

ADR-0017's crypto service needs a physical carrier to move a request/response buffer between
the CM4 (FBL) and the CM0+ (crypto). This is the transport layer of that stack (ADR-0017 D3) —
the cross-core analogue of ISO-TP in M3 (ADR-0013): it moves an **opaque buffer** and knows
nothing about the op it carries. `CLAUDE.md` mandates **hardware semaphores** for IPC, so the
semaphore is a given; the design question is what it guards and how the two cores signal.

## Decisions

### D1. Shared-RAM mailbox for the payload, IPC channel for the doorbell + pointer

The request and response live in a **fixed shared-RAM mailbox** both cores map — the same
pattern the M1 `.noinit` handshake region already established (ADR-0007): a known-address
region carved by the linker, visible to both images. The chip's **IPC message-register
channel** carries only the **doorbell (notify) + a pointer** to that mailbox, not the bytes.

Why split it: the payload (a signature, and under the D1 fallback of ADR-0017, streamed flash
blocks) exceeds an IPC channel register's width, so the channel can't be the primary carrier —
but the channel's built-in **notify interrupt** is exactly how you wake the M0+ without it
spin-polling shared RAM. So: **channel = pointer + doorbell, shared RAM = bytes, semaphore =
ownership** *(exact IPC channel/semaphore/notify mechanics: verify in TRM)*.

**Alternative:** pass the whole payload through the IPC message registers (fixed small width).
Rejected as primary — too narrow for a signature/stream. **Alternative:** M0+ spin-polls a
"request-pending" flag in shared RAM, no channel interrupt. Rejected — a busy-wait that burns
the M0+ and still needs the semaphore anyway; the notify is free and cleaner.

### D2. The hardware semaphore guards mailbox ownership, acquired on both sides symmetrically

A single hardware semaphore denotes "who owns the mailbox right now." The protocol is
explicit and symmetric so it can't rot into a subtle race:

```
FBL (CM4)                          M0+
  acquire(sema)
  write request into mailbox
  set channel pointer + notify  ──▶  (notify ISR) acquire(sema)
  (blocking wait, with timeout)      read request, run op (ADR-0017 D1)
                                     write response into mailbox
  (notified) ◀── notify + release    release(sema)
  read response, release(sema)
```

The acquire/release discipline is **designed, not implicit** — the classic shared-memory bug
is "works in bring-up, fails intermittently later" when one side skips the semaphore or holds
it across the notify. The protocol above is the artifact host tests assert against (D-host).

### D3. Static, single-outstanding request — no queue, no concurrency

One request in flight at a time (the FBL blocks for the verdict, ADR-0017 D2), so the mailbox
is a **single static buffer pair**, no ring, no queue — no heap, MISRA-clean (ADR-0003), one
thing to host-test. This matches ADR-0013 D1's single-session ISO-TP choice for the same
reason: the FBL has exactly one tester / one verify at a time.

### D4. Timeout is the transport's obligation; the *policy* is the caller's

The transport enforces the **N-millisecond bound** on the blocking wait (ADR-0017 D2) using
the same real clock the rest of the FBL uses (`fbl_port_now_ms`, ADR-0008 S2 — the clock is
now genuinely functional after the `PRIMASK` fix). On timeout it returns a distinct **error**
to the caller; it does **not** decide what happens next. *What* a timeout means (⇒ stay in
FBL, ADR-0016 D5) is the verify caller's policy, not the transport's — the layer boundary is
"transport reports, caller decides," same separation as ISO-TP not knowing what a UDS timeout
means (ADR-0013 D3 vs ADR-0014).

### D5. Mailbox placement is a linker concern shared by both images

The mailbox region is fixed at a known address in a part of SRAM **both** the CM4 and CM0+
linker scripts reserve and neither zero-initializes underneath the other — analogous to the
`.noinit` handshake carve-out (ADR-0007), but this region is *cross-core* rather than
*cross-reset*. Exact address within the CM0+/CM4 SRAM split (overview §6) is set when the M0+
image's linker script lands (ADR-0017 D4) *(verify placement against both linker scripts)*.

## Host/target split (ADR-0001)

| Host-testable core | Target-only |
|---|---|
| The acquire→write→notify→await→read→release protocol (D2) as pure state over fake sema/mailbox/clock | the real HW semaphore (`Cy_IPC_Sema_*` or equiv.) |
| Single-outstanding bookkeeping (D3) | the real IPC channel + notify ISR |
| Timeout bounding + error return (D4) | mailbox linker placement (D5) |

The protocol logic is host-tested with a fake semaphore (grant/deny on cue), a fake mailbox
(a plain buffer), and a fake clock — a "M0+ never answers" test drives the D4 timeout with no
target code, feeding ADR-0016 D5's fail-safe test.

## Consequences

- (+) Reuses two established patterns — the `.noinit`-style shared carve-out (ADR-0007) and the
  single-outstanding/static-buffer discipline (ADR-0013 D1) — so it adds little new idiom.
- (+) The notify interrupt avoids an M0+ busy-wait; the semaphore makes ownership explicit.
- (+) "Transport reports, caller decides" keeps the timeout policy out of the transport,
  preserving the layer boundary M3 established.
- (−) A fixed cross-core mailbox address couples the two linker scripts (D5) — a real
  maintenance point, but no worse than the existing `.noinit` coupling.
- (−) Single-outstanding (D3) precludes concurrent crypto ops — fine for M4/M5's blocking
  callers; revisit only if a concurrent user appears.

## Alternatives considered

- **Payload in IPC registers** — too narrow (D1).
- **Spin-polled shared-RAM flag, no notify** — wasteful busy-wait (D1).
- **Queue / multiple outstanding requests** — rejected (D3); no concurrent caller exists.
- **Transport owns the timeout *policy* (resets or retries itself)** — rejected (D4); that
  leaks caller policy into the transport and would let it, e.g., reset the ECU on a crypto
  hiccup without the boot-decision tree's say-so.

## To verify in the TRM / on silicon

- IPC channel + semaphore + notify register mechanics and the CM0+ notify ISR wiring (D1/D2).
- Mailbox address within the CM4/CM0+ SRAM split and the two linker scripts (D5).
- That `fbl_port_now_ms` drives the D4 timeout correctly on the target (it now does after
  ADR-0008 S2, but this is the first cross-core consumer of it).

## Review history

Design-reviewed with the project owner (this session) as the transport layer of ADR-0017.

**Refinement during implementation (host core, `shared/crypto`).** D1 framed the IPC channel
as carrying "the doorbell + a pointer". Implementing the payload-agnostic transport surfaced a
gap: a truly opaque transport (D1) cannot recover the response *length* by parsing the payload
(that would be a layer violation), and the request side needs the same. So the port vtable
carries the byte count next to the doorbell — `notify(size_t req_len)` and a
`response_len()` — rather than a length header inside the mailbox buffer. This is consistent
with D1's intent (the channel register sits beside the pointer and can hold a small scalar);
it just makes the length an explicit part of the seam instead of implied. No change to D2's
protocol shape or D3/D4. The host tests (`test_ipc_mailbox`, `test_crypto_verify`) pin it down;
the real channel/semaphore/notify wiring remains a bring-up seam, findings appended here as in
ADR-0013.
