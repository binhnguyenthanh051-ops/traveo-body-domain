# M3 Reprogramming — Bring-up Log

A live journal of bringing the M3 UDS reprogramming stack (ISO-TP transport →
UDS session + security access → download/routine handlers → the target flash
port) up on the CYT2B7 (TRAVEO™ T2G Body Entry). As in M1/M2, the whole
diagnostic core — ISO-TP segmentation/reassembly, the session + seed/key state
machines, service dispatch, download bookkeeping, the CRC verify — was proven on
x86 under Unity (**119 tests, green**) *before* the board. The layering held its
promise: across all seven bring-up seams, **not one silicon fault required a
change to the host-tested core** — every bug was in a target port or the runtime
environment the stack executes in.

Format per entry: *what I expected · what the silicon did · the lesson*. Brought
up one seam at a time, so every fault stayed answerable to "which seam?". The
milestone deliverable is the last one: a PC tool reflashes the app over CAN and
the FBL verifies and boots it.

---

## Seam 1 — the FBL's polled CANFD driver

The App (M2) reaches CAN through an ISR→queue→task path; the FBL is a super-loop
(ADR-0004), so its driver is **polled** — a deliberate fork, not a copy.

### 1. `Cy_CANFD_GetFIFOTop` returns SUCCESS with an all-zero payload

- **Expected:** poll the RX FIFO fill level (`RXF0S.F0FL`), then read the top
  element with `Cy_CANFD_GetFIFOTop` — the obvious "get the FIFO's top" call.
- **Silicon:** `g_can_rx_count` climbed (frames *were* arriving), but every
  payload read back as zeros — and `GetFIFOTop` returned `CY_CANFD_SUCCESS`, so
  nothing flagged it. `GetFIFOTop` only works when the FIFO **top-pointer logic**
  (`RXFTOP_CTL.F0TPE`) is enabled, which the Device Configurator leaves **off**
  (for this project *and* the App). With it off, that register isn't the live
  read path.
- **Fix:** use `Cy_CANFD_ExtractMsgFromRXBuffer`, which branches on `F0TPE`
  internally — when top-pointer mode is off it computes the real element address
  (`CalcRxFifoAdrs` + `GetRxBuffer`) and acks via `RXF0A`. The App never hit this
  because `Cy_CANFD_IrqHandler` (its ISR path) already dispatches through the same
  correct routine.
- **Lesson:** a PDL call named for what you want isn't automatically the one that
  works in your configuration — and a `SUCCESS` return over garbage data is worse
  than an error. The debug counter (`g_can_rx_count` up, payload zero) split
  "frame not arriving" from "frame read wrong" in one glance.

*(A prior draft also used the TX-side struct types `cy_stc_canfd_t0/t1_t` for the
RX buffer instead of `r0/r1_t` — caught at compile time, before the board.)*

## Seam 2 — ISO-TP over the real bus

Echo a multi-frame message back through `isotp_send` to prove segmentation and
reassembly on silicon, both directions.

### 2. One TX buffer + no wait = the second frame vanishes

- **Expected:** `Cy_CANFD_UpdateAndTransmitMsgBuffer` per frame; the consecutive
  frames of a segmented message just go out in a row.
- **Silicon:** the first consecutive frame reached the bus, the second didn't.
  With a single dedicated TX buffer (ADR-0011 D4), issuing the next transmit
  before the previous request's `TXBRP` (TX Buffer Request Pending) bit clears
  drops or corrupts the in-flight frame. M2's one-frame echo never sent two in a
  row, so this was invisible until ISO-TP's CF burst.
- **Fix:** poll `TXBRP` clear (bounded) before reusing the buffer. Later reused
  the *same* wait, in the other direction, before the `ECUReset` reset needs the
  response to have actually left the buffer (Seam 4).
- **Lesson:** "transmit" only *requests* transmission; a one-buffer channel needs
  an explicit not-busy check before the next request. Fine as long as nothing
  sends back-to-back — until something does.

### 3. CAN FD pads to the next valid DLC, and the receiver believed the padding

- **Expected:** a frame's reported length is its payload length; copy that many
  bytes.
- **Silicon:** a 162-byte round trip came back with extra `0x00`s spliced in.
  CAN FD DLC only encodes specific lengths (0–8, then 12/16/20/24/32/48/64); a
  frame whose real payload lands between steps is **padded up on the wire**.
  Trusting the received frame's length captured that padding as message content.
- **Fix:** size the first-frame chunk to a value that's itself a valid FD length
  (5-byte header + 59 = 64), and derive each consecutive frame's real byte count
  from the ISO-TP *logical* state (bytes still needed against the declared total),
  never from the physical frame length. Regression-tested on host
  (`test_rx_cf_ignores_dlc_padding_beyond_logical_length`); the PC probe had the
  mirror-image bug and was fixed the same way.
- **Lesson:** on CAN FD, physical frame length ≠ logical payload length. Transport
  reassembly must be driven by the protocol's own length accounting, not the
  wire's.

*(Also caught while wiring this seam, before the board: `isotp_send` never set the
frame ID, so every outgoing frame would have gone out as ID 0 — the host tests
only checked payloads, not IDs. Fixed by making `isotp_init` take the response ID,
and added ID assertions to the tests.)*

## Seam 3 — the knock window, for real

The M1 knock hook (`fbl_port_tool_contact`) becomes a real inbound-frame check;
this is the first time the window actually opens and gates on silicon.

### 4. The lifecycle stub was slamming the door it gates

- **Expected:** send a frame on the diagnostic ID during the window; the FBL stays
  resident.
- **Silicon:** it never stayed — the app booted immediately, window or not.
  `fbl_port_lifecycle()` was still the M1 stub hardcoded to `FBL_LC_SECURE` (a
  correct *safe default* while nothing needed the window, since SECURE closes the
  dev backdoor). But M3 needs it open, and ADR-0008 D5's own intent is that a dev
  board sits **pre-SECURE** through M1–M4.
- **Fix:** a real read of `CPUSS_PROTECTION.STATE` (`0x402020C4`, confirmed against
  the register doc), mapping the five raw states onto `fbl_lifecycle_t` with an
  ambiguous/terminal → SECURE safe bias.
- **Lesson:** a safe-default stub is correct right up until the feature it gates
  ships — then the stub *is* the bug. And the value came from the TRM/register
  doc, not a guess.

### 5. SysTick counted in hardware but its interrupt never fired — so the dwell never timed out

- **Expected:** with lifecycle fixed, a *no-knock* boot waits ~2 s and proceeds.
- **Silicon:** with no CAN traffic it hung **forever** — and only started behaving
  once traffic arrived, which made no sense for a *timeout*. Tracing: `s_tick_ms`
  stuck at 0, yet `SysTick->CTRL` showed enable/tickint/clksource all set and the
  counter running. The core register told the truth: **`PRIMASK = 1`**. The vendor
  startup leaves interrupts globally masked after `SystemInit`/`cybsp_init`,
  expecting whatever runs next to unmask them — an RTOS scheduler start does this
  for the App for free; the bare-metal FBL never did. SysTick counted but its
  exception was never taken, so `fbl_port_now_ms()` never advanced and the dwell's
  timeout branch could only ever be escaped by a knock.
- **Fix:** an explicit `__enable_irq()` in `fbl_main()` after `cybsp_init()`.
- **Lesson:** "SysTick is configured" ≠ "SysTick fires" — check `PRIMASK`, not just
  the peripheral. This same masked clock had also quietly disabled ISO-TP's `N_Cr`
  timeout through Seam 2; a prompt success just never needed it. A bare-metal image
  owns the interrupt-enable that an RTOS would have done for it.

## Seam 4 — the real UDS session (0x10, 0x11)

Replace the Seam-2 echo scaffolding with the real dispatch table.
`DiagnosticSessionControl` and the unknown-SID negative-response worked first try;
`ECUReset` did not.

### 6. The reset cut off its own positive response

- **Expected:** `ECUReset` sends its positive response, then resets.
- **Silicon:** every other check passed, but the tester saw no response to
  `ECUReset` — the FBL reset before the frame reached the bus. `isotp_send` only
  *requests* transmission (Seam 2's lesson, other direction); calling
  `fbl_port_system_reset()` immediately after cut the in-flight response short.
- **Fix:** wait for the TX buffer to actually drain (`fbl_can_wait_tx_complete`,
  the Seam-2 `TXBRP` wait reused) before the reset.
- **Lesson:** "send then act" needs the send to have *happened*; a reset is the
  most unforgiving "act" there is.

## Seam 5 — SecurityAccess (0x27)

Seed/key round trip, wrong-key relock, sequence-error cases. **Clean on the first
hardware run** — the only bug was an index error in my *test script* reading the
response. The one seam where the target code was right the first time; worth
noting that it's the one whose state machine was the most thoroughly host-tested
before the board.

## Seam 6 — the target flash port

Erase/program the app-image region directly, behind `hal_flash_if_t`, before
wiring it through UDS.

### 7. Wrong flash-IP family — caught by the compiler

- **Expected:** `Cy_Flash_WriteRow` / `Cy_Flash_EraseRow` (the API I'd read).
- **Silicon (compile-time):** those don't exist for this part. It's the **ECT
  flash IP** (`CPUSS_FLASHC_ECT == 1` → `CY_IP_MXFLASHC_VERSION_ECT`), whose API is
  `Cy_Flash_ProgramRow` + `Cy_Flash_EraseSector`. I'd assumed the non-ECT branch
  because I couldn't get the preprocessor to resolve the guards offline; the
  compile error settled it.
- **Lesson:** when you can't confirm which conditional-compilation branch applies,
  the compiler will — don't guess the branch, let it fail loudly and read which
  symbols it says are missing.

### 8. Mixed sector geometry, with the small sectors at the *top*

- **Expected:** the app region is uniform sectors; one `sector_size` describes it.
- **Silicon (TRM / bench-confirmed):** code flash is **30 large 32 KB sectors then
  16 small 8 KB sectors**, and the small ones sit at the **top** of code flash —
  i.e. the top of the app region — not a don't-care area at the start. A single
  fixed sector size can't describe it; a first draft would have mis-erased the last
  sectors (a 32 KB-aligned erase spanning four 8 KB sectors).
- **Fix:** `erase_sector` erases the real per-address sector (32 KB or 8 KB);
  `erase_range` walks real sector sizes internally; the coarse 32 KB is the
  caller-facing alignment unit (a 32 KB boundary is a valid boundary in both
  regions). Program stays a uniform 512-byte row.
- **Lesson:** "sector size" is not a scalar on this flash. Confirm the sector map
  from the TRM (the user had it), don't infer it from the total size — and never
  assume the irregular region is where it's convenient.

### 9. Flash writes are gated by a safety register that defaults to *disabled*

- **Expected:** erase a sector, program a row — done.
- **Silicon:** both returned `-1`. Capturing the raw PDL status (its low byte is
  the specific error) gave `0x0C` = `CY_FLASH_DRV_FLASH_SAFTEY_ENABLED`: "writes
  disabled in the safety register." The controller refuses all program/erase until
  main-flash writes are explicitly enabled.
- **Fix:** `Cy_Flashc_MainWriteEnable()` (`fbl_flash_init()`), called **only** on
  the programming-mode path — never on the boot/jump path, so code-flash writes
  stay disabled whenever control is handed to the app.
- **Lesson:** don't collapse a rich driver status into a bare `-1` during bring-up
  — the exact code named the fix immediately. And on this part, "the flash is
  writable" is an explicit, revocable choice, not the default.

## Seam 7 — the full download (the milestone)

PC tool → programming session → security → erase → requestDownload/transferData/
transferExit → CRC verify → ECUReset → the FBL verifies and jumps. This is where
the whole stack runs together, and it broke in three genuinely-new places.

### 10. The LED heartbeat starved the transport

- **Expected:** the programming-mode super-loop polls ISO-TP each iteration.
- **Silicon:** requestDownload succeeded, then the first 2 KB block timed out with
  no response. The loop blinked the LED with `Cy_SysLib_Delay(250)` — so
  `isotp_poll()` ran **4 times a second**. A multi-frame `transferData` block
  bursts ~30+ frames far faster than that, and the RX FIFO is 8 deep, so it
  overflowed mid-block and reassembly never completed.
- **Fix:** tight loop — poll every iteration, drive the heartbeat off the
  millisecond clock instead of a blocking delay.
- **Lesson:** the classic "never block in the super-loop," and a vindication of the
  poll-based transport choice (ADR-0012 D2): the fix was purely *how often* the
  existing poll runs — no structural change, no ISR retrofit.

### 11. A HardFault from fetching code out of the flash being programmed

- **Expected:** with polling fixed, the block reassembles and the download writes
  it to flash.
- **Silicon:** HardFault. The fault frame decoded to `iBusErr` (instruction bus
  error) + `forced`, with `sysTickAct = 1`. A blocking flash program/erase busies
  the code-flash macro, and **fetching an instruction from that macro while it's
  busy is a read-while-write violation** → fault. The synchronous flash path runs
  from SROM/ROM (safe); the trigger was **SysTick firing mid-operation and
  vectoring to its flash-resident handler**. Seam 6's isolated test didn't hit it
  because its target (the top small sector) was far from the FBL's code; Seam 7's
  target (`0x1004_0000`, right next to the FBL) is in the neighbouring sector
  group.
- **Fix:** mask interrupts (`Cy_SysLib_Enter/ExitCriticalSection`) around each
  blocking flash call. Safe here — the PC is waiting for our response during the
  op, so no CAN RX is missed.
- **Lesson:** on a bootloader that erases the flash it lives in, an ISR whose
  handler is in flash is a latent HardFault during any flash op. The decoded fault
  frame (`iBusErr` + `sysTickAct`) pointed straight at the mechanism.

### 12. ECUReset counted a deliberate reset as a crash

- **Expected:** after a verified download, `ECUReset` → the boot decision jumps to
  the new app.
- **Silicon:** it stayed resident; only a power cycle booted the app. Two layers,
  peeled in order:
  - First (host-tool): the tool closed the CAN bus immediately after the
    no-response `ECUReset` frame, before `python-can` transmitted it — so the reset
    never happened at all. A `0.5 s` flush fixed that.
  - Then, with `ECUReset` actually resetting: a real design bug. D10 clears the
    `.noinit` programming-request *before* the reset (so the boot can jump, not
    stay resident) — but that request is the **very signal D4 uses to treat a
    deliberate reflash as not-a-crash**. So the post-reset boot counted the
    deliberate reset as a crash software-reset and incremented the boot-loop
    counter; repeated download cycles climbed it past the threshold and trapped a
    *valid* app in the FBL.
- **Fix:** the `ECUReset` sequence also clears the BREG boot-loop counter — D4's
  "deliberate reflash → clear" intent, applied where the `.noinit` signal can no
  longer carry it (ADR-0007 D10a).
- **Lesson:** clearing a signal to satisfy one consumer (the boot decision) can
  silently starve another (the counter rule) that read the same signal. And a
  no-response message needs the bus kept alive until it's physically on the wire —
  the same "send ≠ sent" lesson as Seam 2/4, one layer up in the tooling.

---

## Where M3 stands

All seven seams up on silicon. The headline: **a PC-side flash tool
(`host_tools/uds_flash`) reflashes the Node A app over CAN** — ISO-TP transport,
UDS programming session, seed/key security access, sector erase, block download,
CRC32 verify, `ECUReset` — after which the FBL's own boot decision re-verifies the
freshly-written image and jumps to it. Confirmed end to end: the app's LED returns
on its own, no power cycle.

Twelve findings, and the pattern the brief predicted held exactly: the layered,
host-tested diagnostic core never changed during bring-up. Every fault lived in a
target port (CAN extraction, TX flow control, flash IP/geometry/safety, the
lifecycle read) or in the runtime the stack runs in (PRIMASK, the super-loop
cadence, read-while-write, the boot-counter interaction) — exactly the seam the
host tests can't reach, and exactly where the design put the boundary. **M3
bring-up is complete.**
