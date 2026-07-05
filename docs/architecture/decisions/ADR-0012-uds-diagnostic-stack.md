# ADR-0012: UDS diagnostic-stack layering (M3)

**Status:** accepted · **Date:** 2026-07-02

## Context

M3 makes the FBL reprogrammable over the bus: a PC tool drives a UDS download that writes a
new application image, which the FBL then verifies and jumps to. M3/M4/M5 are one layered
diagnostic-and-security stack designed once, not three separate features — later milestones
(M4 signature verification, M5 SecOC, later app-side data/DTC diagnostics reusing the same
server) must slot in as modules behind stable interfaces.

The stack borrows the **AUTOSAR diagnostic layering** as a reference model, without
implementing AUTOSAR (no RTE, no generated BSW). The FBL is **super-loop, not RTOS**
(ADR-0004) — the whole stack runs synchronously inside `fbl_port_enter_programming_mode()`,
which today (M1) is a stub LED-blink loop.

## Decisions

### D1. Four layers, strict one-directional dependency

```
Transport (ISO-TP)  ← Session (UDS server)  ← Service handlers  ← Operations / back end
```

- **Transport** segments/reassembles CANFD frames into a complete buffer; hands it up; never
  sees a service ID and is never told one.
- **Session** ("DCM"-like) owns session-type state (default/programming) and security-access
  state (locked/seed-sent/unlocked), P2/S3 timing, and SID dispatch. It depends on the
  transport interface (downward) and on the **handler interface** (an abstraction it calls
  through, never a concrete handler module by name).
- **Service handlers** — one per SID behind a common interface (0x10, 0x27, 0x34/0x36/0x37,
  0x31, 0x11). A handler touches only the request/response buffers, a read-only
  session-context query, and the operations layer below it — never CAN, never ISO-TP, never
  another handler's state.
- **Operations / back end** — flash program/erase/read, the image verify, the reset. No UDS
  semantics live here.

The composition root (`fbl_main.c` / a `diag_init()`) is the only place that names both
concrete handlers and concrete operations implementations — consistent with ADR-0004's
per-variant config pattern.

### D2. Transport is poll-based, not ISR/queue

`isotp_poll(now_ms)` is called once per FBL super-loop iteration; it drains available CAN RX
and advances SF/FF/CF/FC state. This is a deliberate departure from the App's ADR-0010/0011
ISR→queue→task pattern — that pattern fits FreeRTOS; the FBL has no scheduler to hand work
to, so an event-callback API would just need an artificial "pending" flag polled anyway.
Full timing/config: ADR-0013.

### D3. Session dispatch is a static, config-time table — not runtime registration

SID → `uds_handler_if_t*` is a `static const` array wired at build time, not a
`uds_register_handler()` API. No heap, deterministic, and the whole table is one thing to
host-test. *Alternative considered:* a registration API is more extensible and is closer to
what the App will eventually want when it reuses this same server for data/DTC services —
deferred until that reuse is real, not built speculatively for the FBL.

### D4. Download bookkeeping belongs to the transfer handlers, not the session

Address, block-sequence counter, remaining size, and staged-write state for
`requestDownload`/`transferData`/`requestTransferExit` are owned by that handler cluster, not
folded into session state. Session state machines (session-type, security-access) are
generic and reusable by any future service; download progress is specific to one cluster.
Conflating them would let an unrelated service (e.g. 0x27 SecurityAccess) observe or mutate
download progress it has no business touching.

### D5. Interrupted-download recovery reuses the existing ADR-0008 fail-safe — no new mechanism

A power loss mid-`transferData` must never let a partial image boot. This does **not** need
new machinery: ADR-0008 D1 step 5 ("invalid/blank app ⇒ stay in FBL, never jump") already
covers it, because a partial image leaves the header incomplete or the trailer unwritten,
which the existing digest/vector check already rejects on the next boot. The download
bookkeeping's only obligation (D4) is to never let `requestTransferExit` finalize on a
gap/out-of-order block/incomplete sequence — it does not need to reconstruct or resume a
partial transfer; a fresh `requestDownload` restarts cleanly (no resumable-download support
in M3 scope).

### D6. Reuse existing seams; add exactly one interface type and one struct extension

- **Transport** rides the existing `can_hal_if_t` (ADR-0011) unchanged.
- **Reset / time / tool-contact** reuse the existing `fbl_port_*` singleton pattern
  (`fbl_port_system_reset`, `fbl_port_now_ms` as-is; `fbl_port_tool_contact` unchanged in
  signature, per ADR-0008 D2's own "the core does not change" — only its target
  implementation grows from a fixed-ID check to a real inbound-frame check). See ADR-0007
  D10 for the one genuinely new port-crossing behaviour M3 adds (the FBL becomes a writer of
  `.noinit`).
- **Flash** reuses `hal_flash_if_t` (`shared/hal`), extended with one optional field:
  `int (*erase_range)(uint32_t addr, uint32_t len)` (nullable — NULL means "no HW bulk-erase;
  caller loops `erase_sector`") and `uint32_t program_row_size`. `write()` already accepts
  arbitrary length, so a "burst" program needs no interface change. Alignment/chunking
  policy (how a `transferData` block maps onto row/sector boundaries) is decided by the
  download-bookkeeping object (D4), not the port.
- **Verify** reuses `shared/boot`'s existing `fbl_digest()` / `fbl_app_image_valid()`
  (ADR-0008 D3) **directly** — called from the new `routineControl` "check programmed image"
  handler. **No new `verify_if_t` interface is introduced.** ADR-0008 D3 already solved
  "CRC32 now, SHA-256 in M4" via a compile-time macro (`FBL_DIGEST_ALGO`); since the
  boot-time jump-check and the UDS-triggered image check live in the *same* FBL image built
  with the *same* macro, a second, runtime-swappable selection axis would duplicate the
  first for no benefit. The Strategy is real — M4 flips one macro and neither call site
  changes — it is just resolved at compile time, not through a function-pointer struct.

## Host/target split (ADR-0001)

| Host-testable core | Target-only (behind ports) |
|---|---|
| ISO-TP segmentation/reassembly (frame logic) | CANFD send/receive (existing `can_hal_if_t`) |
| Session state machines + SID dispatch | — |
| Each handler's logic, incl. download bookkeeping | flash program/erase/read driver |
| `fbl_digest()` / `fbl_app_image_valid()` (reused, unchanged) | the actual reset |

## Consequences

- (+) Exactly one new interface type (`verify_if_t` avoided; only the `hal_flash_if_t`
  extension and the diag stack itself are new surface).
- (+) M4/M5 slot in without touching the session or transport: M4 flips `FBL_DIGEST_ALGO`;
  M5's SecOC is an operations-layer concern, not a diagnostic-stack one.
- (+) Interrupted-download safety costs nothing new — it rides the existing boot-decision
  fail-safe.
- (−) Static dispatch table means adding a service is a build-time change, not a runtime one
  — acceptable for a single fixed FBL image; revisit if the App's reuse needs registration.

## Alternatives considered

- Callback-push transport (ISR-style, like the App) — rejected; doesn't fit the FBL
  super-loop (ADR-0004).
- Runtime handler registration — rejected for the FBL; no heap, one fixed image, one call
  site wiring it.
- A parallel `verify_if_t` Strategy struct — rejected as a duplicate selection mechanism for
  a concern ADR-0008 D3 already resolved at compile time.
- Global download-bookkeeping folded into session state — rejected; wrong ownership (D4).

## Review history

Design-reviewed in discussion before implementation (this session). Module skeletons +
failing Unity tests, then implementation, then seam-by-seam FBL bring-up on real silicon.

The D6 flash-operation interface (`hal_flash_if_t` bound to the app-image region,
`node_a_gateway/bootloader/src/port_flash.c`) surfaced three silicon findings during M3
Seam 6 bring-up — the design's "target-only, behind a port" boundary held, but the port's
first draft made assumptions the compiler and then the chip falsified:

- **S1** (silicon) — this part is the **ECT flash IP** (`CPUSS_FLASHC_ECT == 1` →
  `CY_IP_MXFLASHC_VERSION_ECT`), whose API is `Cy_Flash_ProgramRow` + `Cy_Flash_EraseSector`.
  `Cy_Flash_WriteRow`/`Cy_Flash_EraseRow` (the non-ECT pair the first draft used, when the
  preprocessor couldn't be run to check) **do not exist** for ECT — caught at compile time.
- **S2** (silicon) — code flash is **mixed sector geometry**: 30 large 32 KB sectors then 16
  small 8 KB sectors, and the small sectors sit at the **top** of code flash — i.e. the top of
  the app image region (`0x100F_0000`–`0x1011_0000`), not a don't-care area at the start as the
  first draft assumed. A single fixed `sector_size` can't describe this; the port erases the
  real sector at each address (`fbl_flash_sector_size()`), reports the coarse 32 KB granularity
  as the caller-facing alignment unit (a 32 KB boundary is a valid sector boundary in both
  regions), and `erase_range()` walks real sector sizes internally. **This confirms D6's
  intent**: erase and program are genuinely different, non-uniform granularities, which the
  download bookkeeping (D4) respects by erasing the whole region and programming row by row.
- **S3** (silicon) — program/erase of main (code) flash is gated by a **write-safety register**
  that defaults to disabled; the SROM returns `CY_FLASH_DRV_FLASH_SAFTEY_ENABLED` until
  `Cy_Flashc_MainWriteEnable()` is called. The FBL lifts it (`fbl_flash_init()`) only on the
  programming-mode path, never on the boot/jump path, so code-flash writes stay disabled
  whenever control is handed to the app.

The full end-to-end download (Seam 7 — the milestone deliverable: PC tool → the whole UDS
programming sequence → the FBL verifies and jumps to the new app) surfaced two more findings
in the FBL *runtime* the stack executes in, both about the D2 super-loop rather than the
diagnostic logic (which was host-tested and unchanged):

- **S4** (silicon) — a blocking flash program/erase busies the code-flash macro, and
  **fetching an instruction from that macro while it is busy is a read-while-write violation
  → HardFault** (the fault frame showed `iBusErr` + `sysTickAct`). The synchronous flash-op
  path runs from SROM/ROM, so it is safe; the trigger was **SysTick firing mid-operation and
  vectoring to its flash-resident handler**. Fixed by masking interrupts
  (`Cy_SysLib_Enter/ExitCriticalSection`) around each blocking flash call in `port_flash.c`.
  Only surfaced in Seam 7, not Seam 6's isolated flash test, because Seam 7's target
  (`0x1004_0000`, adjacent to the FBL's own code) is in the same/neighbouring sector group
  as the code being fetched; Seam 6's target (the top small sector) was far away.
- **S5** (silicon) — the programming-mode super-loop's LED heartbeat used a blocking
  `Cy_SysLib_Delay(250)`, which **starved `isotp_poll()` to once per 250 ms**. A multi-frame
  `transferData` block bursts ~30+ consecutive frames far faster than that, and the CAN RX
  FIFO is only 8 deep, so it overflowed mid-block and reassembly never completed
  (requestDownload succeeded, then the first 2 KB block timed out). Fixed by making the loop
  tight — poll every iteration, drive the heartbeat off the millisecond clock instead of a
  blocking delay. The classic "never block in the super-loop" bootloader rule, and a concrete
  vindication of D2's poll-based-transport choice: the fix is purely *how often* the existing
  poll runs, no structural change.

*(Two host-tool bugs also turned up here and were fixed in `host_tools/uds_flash/` rather
than the firmware: a response-index off-by-one reading the routineControl check-image status,
and closing the CAN bus before the no-response `ECUReset` frame transmitted — see ADR-0007
D10a for the latter, which also masked the boot-loop-counter interaction until fixed.)*
