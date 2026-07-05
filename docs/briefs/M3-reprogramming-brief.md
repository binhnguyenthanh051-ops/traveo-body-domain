# M3 — UDS reprogramming in the FBL (architecture-first brief)

> **Role:** act as an **automotive diagnostic-stack architect**, not a coder. The FIRST and
> ONLY deliverable of the opening session is a **layered architecture design** — no service
> code, no download sequence yet. Tradeoffs + at least one alternative at every layer
> boundary (the reasoning is the blog/interview material and the point of the milestone).
> Same rhythm afterwards: discuss → agree → ADR(s) → failing host tests → implement →
> on-board bring-up.

## Where this sits

M1 (FBL boots→verifies→jumps) and M2 (Node A app on FreeRTOS; CANFD live on real hardware;
App-side `.noinit` programming-request closes the loop into the FBL) are **done and tagged**.
M3 makes the FBL **reprogrammable over the bus**: a PC tool drives a UDS download that writes
a new application image into flash, which the FBL then verifies and jumps to.

**Primary goal is architectural, not featural.** M3/M4/M5 are not four features — they are one
**layered diagnostic-and-security stack** that must be designed *once, now*, so later milestones
slot in as modules behind stable interfaces instead of being surgery on working code. Designing
that stack well is the top-tier architecture artifact of this whole project.

## The architecture to design FIRST (the whole point of the opening session)

Borrow the **AUTOSAR diagnostic layering** as a reference model — **without implementing
AUTOSAR**. Adopting a proven layering and justifying it *is* the architect move. Design these
layers with a strict, one-directional dependency graph (upper depends on lower via interfaces;
never sideways; lower layers know nothing about upper):

1. **Transport (ISO-TP / CAN-TP).** Segmentation/reassembly of multi-frame UDS messages over
   CANFD (single/first/consecutive/flow-control frames, block size, STmin). Knows nothing about
   *which* service it carries. Hands the layer above a **complete reassembled buffer**; the layer
   above must never see a CAN frame.
2. **Session / protocol (UDS server — "DCM"-like).** Session state machine (default vs
   programming), security-access state (locked → seed → unlocked), service dispatch, timing
   (P2/S3). Knows UDS; knows nothing about CAN framing below *or* what any service does above.
3. **Service handlers.** One handler per service behind a **common handler interface**
   (0x10 session control, 0x27 security access, 0x34 requestDownload, 0x36 transferData,
   0x37 requestTransferExit, 0x31 routineControl, 0x11 ECUReset). Adding a service = adding a
   handler, not editing the server.
4. **Operations / back end.** The actual effects behind interfaces: flash program/erase, the
   image **verify**, the reset. The *same* UDS server drives a CRC32 verify in M3 and a signature
   verify in M4 **without the server changing**.

### The seams that must exist from day one (forward-design, per your header-extensibility habit)

- **The `verify` strategy seam (critical).** Define a single `verify` interface now, with **CRC32
  as its first Strategy implementation**. M4 signature verification becomes a *new strategy plugged
  in by config*, not a refactor of the UDS server. This is the concrete answer to "how do UDS and
  security co-exist cleanly": **security is a strategy plugged into the diagnostic stack, not code
  threaded through it.** Skipping this interface now = M4 is surgery; defining it now = M4 is a plug-in.
- **The flash-operation interface.** Program/erase/read behind a port so the download logic is
  target-independent and host-testable; the real driver is target-only.
- **The transport port.** CAN send/receive behind the existing CAN HAL; ISO-TP logic itself is
  pure and host-tested.

### Patterns to name explicitly (defensible in an interview)

- **Layered architecture** with enforced dependency direction (transport ← session ← handlers ←
  operations). "Bad layering shows up as CAN/flash/FreeRTOS calls leaking into the protocol logic"
  — that leak is the smell to design against.
- **Strategy** for `verify` (CRC32 now, signature in M4) — the security/diagnostic co-existence answer.
- **State pattern** for the session + security-access state machine (host-testable in isolation).
- **Command/handler** for service dispatch (a service = a handler behind one interface).

## Host/target split (ADR-0001 — and a forcing function)

| Host-testable core | Target-only (behind ports) |
|---|---|
| ISO-TP segmentation/reassembly (frame logic) | CANFD send/receive (existing CAN HAL) |
| UDS session + security-access state machine | flash program/erase/read driver |
| Service dispatch + each handler's logic | the actual reset |
| The `verify` strategy (CRC32) over a buffer | (M4) crypto/signature back end |
| Download bookkeeping (addresses, block seq, sizes) | — |

If the layering is right, this split falls out naturally. If FreeRTOS/CAN/flash calls appear in
the protocol logic, the layering is wrong — use that as the design check.

## Scope for M3 (keep it minimal, but architected)

In scope: the layered stack above; ISO-TP transport; UDS server with programming session +
security access (seed/key — a simple algorithm for M3, the *mechanism* is the point);
requestDownload / transferData / requestTransferExit + routineControl (erase + CRC verify);
ECUReset; a **PC-side Python flash tool** (`host_tools/`) that drives the sequence; integration
tests in CI; and the **interrupted-download / power-loss recovery** story (the failure chapter).

**Deliberately deferred** (design the seam, don't build):
- App **signature** verification / real crypto — **M4** (drops into the `verify` strategy).
- SecOC / authenticated messaging — **M5**.
- App-side (data/DTC) diagnostics — later; the *same* UDS server is reused, so design for it.
- A/B banking / rollback — future (the image header already leaves room).
- Full UDS service set — only the services the download flow needs.

## Deliverables (in order)

1. **The layered architecture design** — layers, dependency direction, the interface at each
   boundary, the `verify` strategy seam, tradeoffs + one alternative per boundary. **Stop here;
   wait for agreement.**
2. On agreement: **ADR(s)** — the diagnostic-stack layering; the verify-strategy seam
   (CRC32→signature); ISO-TP config; security-access design. (These are the milestone's real
   artifact.)
3. Module skeleton + interfaces (headers) for each layer, kept host-fakeable.
4. **Failing Unity tests** for the host-testable core — ISO-TP reassembly, the session/security
   state machine, service dispatch, the CRC verify strategy, download bookkeeping. Include the
   **interrupted-transfer** cases (a block lost / out-of-order / power-loss mid-download ⇒ safe,
   resumable or cleanly re-startable state; never a half-flashed app treated as valid).
5. Implement against tests; `make test` + `make lint` green. Target-only (CANFD, flash driver,
   reset) behind ports with host fakes.
6. On-board bring-up: PC flash tool → ISO-TP → UDS programming session → download → CRC verify →
   ECUReset → FBL verifies and jumps to the newly-flashed app. Then the failure demo: pull power
   mid-download, show the FBL refuses the partial image and stays reprogrammable.

Start with step 1 only. Wait for agreement before ADRs or code.

## Watch-fors (call these out, don't gloss)

- **Layer bleed** — the classic hand-rolled-stack failure: ISO-TP knowing about services, or the
  UDS server touching CAN frames. Transport hands up a complete buffer; the server never sees a frame.
- **Design the `verify` seam in M3 even though M3 only does CRC32** — or M4 becomes a rewrite.
- **CRC32 ≠ authenticity** (carry the M1 honesty note) — M3 download integrity is not security;
  authenticity is M4's signature strategy over the same seam.
- **Interrupted download must fail safe** — a partial image must never verify/boot; this ties to
  M1's fail-safe (invalid app ⇒ stay in FBL) and the boot-loop counter.
- **Reprogram-request vs boot-loop counter** — the M2/B2 "sw-reset + `.noinit` pattern ⇒ clear"
  rule stays in force; a download-then-reset must not look like a crash-loop.
- **ISO-TP flow control / timing** (block size, STmin, P2/S3 timeouts) — get the state machine
  right in host tests before trusting it on the bus.
- **ADR-0001 leakage** — no CAN/flash/FreeRTOS calls in the protocol logic; that leak *is* the
  layering bug.

## Blog / architecture note

The headline artifact here is the **layered diagnostic stack** and the **verify-strategy seam** —
"I designed UDS, security, and diagnostics as one layered stack so M4's signature check plugged in
without touching the UDS server" is exactly the architecture-mindset story the whole portfolio is
for. The interrupted-download recovery is the failure chapter. Design docs + the layering diagram
are the deliverable a hiring manager weighs most.
