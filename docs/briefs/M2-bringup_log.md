# M2 Application — Bring-up Log

A live journal of bringing the M2 Node A application (FreeRTOS on CM4F + CANFD +
the `.noinit` reprogram handshake) up on the CYT2B7 (TRAVEO™ T2G Body Entry). As
in M1, the RTOS-independent logic (body-control state machine, message decode,
the `.noinit` encode + the M2-5 boot-loop rule) was proven on x86 under Unity
**before** the board; everything below is what happened at the **silicon / port
seam**.

Format per entry: *what I expected · what the silicon did · the lesson*. Brought
up one seam at a time (FreeRTOS → CANFD → `.noinit`), so every fault stayed
answerable to "which seam?".

---

## Seam 1 — FreeRTOS + heartbeat

### 1. The app's `FreeRTOSConfig.h` fights the library's template

- **Expected:** add the `freertos` middleware, put my `FreeRTOSConfig.h` in
  `config/`, done.
- **Silicon:** the Infineon `freertos` library ships its **own**
  `FreeRTOSConfig.h` under `Source/portable/COMPONENT_CM4/` — the same dir as
  `portmacro.h`, which is on the include path. So `#include "FreeRTOSConfig.h"`
  was ambiguous between mine and the template; include order decides, which is
  fragile.
- **Fix:** a `.cyignore` excluding *only* the library's template file (leaving
  `portmacro.h`/`port.c`), plus excluding `abstraction-rtos`/`clib-support` (their
  heap-backed wrappers fight the static-only policy). Also set
  `configCPU_CLOCK_HZ = SystemCoreClock` (not a hardcoded 160 MHz) so the tick is
  right regardless of the actual clock tree.
- **Lesson:** a vendor RTOS port is not just a port — it's a config *template* you
  must consciously override, and the override has to be deterministic (ignore the
  template), not "hope my include comes first".

### 2. A "stack overflow" that wasn't — and the assert it was hiding

- **Expected:** the first trap, in `vApplicationStackOverflowHook`, meant a task
  stack was too small.
- **Silicon:** `g_overflow_task_name` was `0` (the hook never ran), and the trap
  was reached *during* `vTaskStartScheduler` — **before** any context switch, when
  the overflow check can't fire. The two trap functions
  (`vApplicationStackOverflowHook`, `vAssertCalled`) were **byte-identical**, so
  the linker **folded them to one address** (identical-code-folding) and the
  debugger labelled it the wrong one. The real fault was a **`configASSERT`**.
- **The actual bug:** `configASSERT(SCB->VTOR == APP_VECTOR_BASE)` in my handover
  check. The FBL *does* set `VTOR = app_base` at the jump, but Cypress
  `SystemInit` then **relocates `VTOR` to the RAM vector table** (`.ramVectors`,
  ~`0x0800_8000`) for runtime ISR registration — so by `main()` it is *not* the
  flash app base. The assert was checking the wrong thing.
- **Fix:** accept the RAM-vectors address *or* the flash base; trap only a
  genuinely-wrong table. Made the two hooks capture distinct state so folding
  can't disguise them again.
- **Lesson:** two diagnostic traps with identical bodies are indistinguishable
  after the linker folds them — give each one unique side effects. And "the FBL
  set VTOR" doesn't mean VTOR still points there after the BSP's `SystemInit` runs
  (ADR-0010 D7 corrected).

### 3. CM4F stacks are bigger than reflex

- **Expected:** 128–256-word task stacks are plenty for stub tasks.
- **Silicon:** once stacks were sized for the **CM4F context frame** — the
  hardware exception frame (up to 26 words *with* an FP frame) + `R4–R11` +
  `S16–S31`, ~50 words of pure overhead before the task's own locals — the reflex
  sizes were too tight. Bumped to 256/512 words; trim later from
  `uxTaskGetStackHighWaterMark`.
- **Lesson:** the FP context isn't free even for tasks that "don't use float" — the
  port reserves room for it. Budget the frame, then measure.

## Seam 2 — CANFD echo

### 4. "Init OK, no RX" — the TRAVEO CM4 NVIC mux

- **Expected:** `Cy_SysInt_Init({canfd_irqn, prio 5}, isr)` + `NVIC_EnableIRQ` wires
  the RX interrupt, same as PSoC 6.
- **Silicon:** loopback engaged, FIFO filled, RF0N set *in the peripheral* — but
  no ISR ever fired. On TRAVEO T2G the **CM4 reaches peripheral interrupts through
  an 8-channel NVIC mux** (`NvicMux0..7`); the CANFD IRQ enum
  (`canfd_0_interrupts0_1_IRQn` = 58) is a **system-interrupt index, not a CM4 NVIC
  line**. `intrSrc` must **pack a mux channel** (`<< CY_SYSINT_INTRSRC_MUXIRQ_SHIFT`)
  with the system interrupt, and `NVIC_EnableIRQ` takes the **mux channel**.
  Passing the bare IRQn silently mapped it to `NvicMux0` and enabled a non-existent
  line.
- **How it was localised:** per-stage counters (`tx`/`isr`/`cb`/`rx`) — `tx` climbed,
  `isr` stayed 0 — pinned it to "issued but never reached the NVIC" in one flash.
- **Lesson:** the M1 NVIC-mux warning was real, and I walked into it anyway. On
  TRAVEO, a peripheral IRQ enum is a *system* interrupt that must be routed to one
  of the few CM4 mux channels — it is not a direct NVIC line like PSoC 6 (ADR-0011
  D3). Stage counters beat guessing.

### 5. RX FIFO 0 with zero elements drops every frame

- **Expected:** enabling RX FIFO 0 + an accept-all filter is enough to receive.
- **Silicon:** the configurator generated `rxFifo0Config.numberOfFIFOElements = 0`
  — routing was correct (`nonMatchingFramesStandard = ACCEPT_IN_RXFIFO_0`), but the
  FIFO had **no storage**, so frames were dropped and RF0N never fired.
- **Lesson:** "enable FIFO 0" and "give FIFO 0 elements" are two different
  configurator settings; element count 0 is a silent black hole.

### 6. M_CAN loopback bits are protected — wrap them in config mode

- **Expected:** `Cy_CANFD_TestModeConfig(…INTERNAL_LOOP_BACK)` after `Init` turns on
  internal loopback.
- **Silicon:** the `CCCR.TEST`/`MON` and `TEST.LBCK` bits are **write-protected**
  unless `CCCR.CCE=1 && INIT=1`. After `Init` the channel is operational, so a bare
  call did nothing. Wrapping it in `Cy_CANFD_ConfigChangesEnable` /
  `…Disable` engaged it (verified by reading `CCCR = 0x3A0`, `TEST = 0x90`).
- **Lesson:** protected-register changes on M_CAN need the config-mode dance; PDL
  helpers that "just write the register" don't enter it for you. Internal loopback
  also lets the whole RX path be proven with **zero external hardware** — the right
  first move before the bus.

## Seam 3 — the `.noinit` handshake

### 7. The FBL's real linker wasn't wired

- **Expected:** the FBL builds with the `fbl.ld` I wrote in M1.
- **Silicon:** the FBL Makefile's `LINKER_SCRIPT` was **empty** — the FBL was built
  with the BSP default linker, and `fbl.ld` was inert documentation. Its BSP linker
  *did* already use the correct CM4 RAM origin (`0x0800_8000`) and `0x1002_0000`
  placement, so the fork was just the `.noinit` pin.
- **Fix:** fork the BSP CM4 linker into `fbl_cm4.ld`, pin `.noinit`, set
  `LINKER_SCRIPT` + `-L` (native path on Windows). Re-verified M1 boots.
- **Lesson:** a linker file in the tree isn't the build's linker unless
  `LINKER_SCRIPT` points at it. Check the wiring, not just the file.

### 8. "Pin `.noinit`" means pin the handshake's *own* section

- **Expected:** placing the whole `.noinit` output section at `_noinit_start`
  (`0x0801_F700`) puts the handshake there.
- **Silicon:** the `.map` showed `g_handshake` at **`0x0801_FCA0`**, not the pin.
  `.noinit` is **shared** — BSP `system_psoc6` (~`0x5A0` bytes) + `cyhal` +
  `cy_syslib` land *first*, so our 12-byte handshake sat after them, *inside the
  reserved top-2 KB*. It matched across images only because both had byte-identical
  `.noinit` layouts — a coincidence a BSP/lib bump would silently break.
- **Fix:** a **dedicated `.fbl_handshake` section** pinned at `_noinit_start`; the
  general `.noinit` floats back in `ram` for the BSP/libs. Both images now read the
  handshake at a fixed `0x0801_F700`, guarded by `ASSERT(ADDR(.fbl_handshake) ==
  _noinit_start)`.
- **Lesson:** `.noinit` is a shared catch-all, not yours. To pin *your* data at a
  fixed cross-image address, give it its **own** named section — pinning the
  catch-all just relocates everyone and leaves your variable at an offset. (Caught
  by reading the `.map`, not by the handshake "working" — it worked by luck.)

---

## Where M2 stands

All three seams up on silicon: FreeRTOS + heartbeat, CANFD echo (internal
loopback — full path proven), and the App→FBL `.noinit` reprogram (the deferred M1
thread, closed). Remaining: **real-bus CANFD echo** with the VN1610 (the loopback
proved the software; the bus proves the transceiver + bit timing + a second node's
ACK).
