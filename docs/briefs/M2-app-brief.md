# M2 — Node A application on FreeRTOS

> **First deliverable is a design discussion — do NOT let it jump to implementation.**
> Same rhythm as M1: discuss → agree → ADR(s) → failing host tests → implement → on-board
> bring-up. Tradeoffs and at least one alternative per decision (the reasoning is blog material).

## Where this sits

M1 is done and tagged (`m1-bootloader`): the FBL boots → verifies the app (CRC32) → jumps to a
minimal CM4 app at `0x1004_0000`. M2 turns that **blinky stub into the real Node A (gateway)
application running on FreeRTOS** (ADR-0005: the app runs on FreeRTOS; the `scheduler/` module
is a *standalone deep-dive*, NOT the app kernel and NOT linked into any image).

This is also the **interview-prep milestone**: FreeRTOS on Cortex-M4F is where PendSV,
FPU lazy-stacking, and BASEPRI critical sections actually live. The design discussion should
*explain* those internals, not just configure them — that's the point of doing it by hand once.

## Design discussion (the topics to work through)

- **FreeRTOS port + config.** The `ARM_CM4F` port (hardware FPU). Tick source (SysTick @ a chosen
  `configTICK_RATE_HZ`) on the 160 MHz CM4. **Heap scheme** (heap_4 coalescing vs static-only for
  determinism) and **static vs dynamic allocation** (`configSUPPORT_STATIC_ALLOCATION` for a
  body-domain ECU — argue it). `FreeRTOSConfig.h` essentials: priorities, assert hook, stack-overflow
  check, idle/timer tasks.
- **The interrupt-priority model (get this exactly right).** `configMAX_SYSCALL_INTERRUPT_PRIORITY`
  vs `configKERNEL_INTERRUPT_PRIORITY`, **BASEPRI** masking, and this MCU's `__NVIC_PRIO_BITS`. The
  rule any ISR that calls a `…FromISR` API must obey, and the classic fault when it doesn't. (Explain
  it — interview gold.)
- **Context switch internals (design-essay / interview prep).** PendSV as the lowest-priority
  context-switch exception, SVC for the first task, and **FPU lazy stacking** (when the hardware
  actually pushes S0–S31, and why the port reserves room for it). Tie back to what the standalone
  `scheduler/` deep-dive (EP.06-07) does by hand vs how FreeRTOS does it.
- **Task / ISR architecture.** Propose a *minimal but real* task set and justify priorities + stack
  sizes — e.g. a **health/heartbeat** task (LED + stack/heap watch + later the watchdog), a **CAN-RX**
  task fed by the CAN ISR via a queue/stream-buffer, and an **app-logic** task (the gateway's actual
  body-domain function). Keep M2 small; design the structure for growth. The ISR→task handoff
  (`xQueueSendFromISR` + `portYIELD_FROM_ISR`) is a first-class topic.
- **The FBL→app handover, App side.** The app inherits a *quiesced* state from the FBL (IRQs disabled,
  NVIC cleared, SysTick stopped, `VTOR = app_base`, `MSP = app SP`). So the app re-inits its own
  clocks/peripherals (`cybsp_init`), lets FreeRTOS set the PendSV/SVC/SysTick priorities, and only
  re-enables IRQs when the scheduler starts. Spell out what the app may and may NOT assume the FBL left.
- **The `.noinit` programming-request channel — App side (closes the deferred M1 thread).** M1 deferred
  the on-target `.noinit` work; M2 is its natural home. The App writes a programming-request into the
  shared `.noinit` region and triggers a software reset; the FBL (already built) reads it and stays in
  programming mode. This requires finally **pinning the `.noinit` region in both linkers** (FBL + app)
  at a fixed address the startup does not zero, plus the **ECC-priming** behaviour M1 flagged
  (*verify ECC-on-uninitialised-read in TRM*). End-to-end App-requests-reprogramming is the M2 proof.
- **CAN bring-up.** The gateway needs CAN (the TRAVEO T2G **CANFD** IP — message RAM, RX filters, TX).
  Minimal for M2: one channel, a bitrate, RX→ISR→queue, TX. *(This also unblocks the deferred M1 CAN
  knock window — same peripheral, a fixed knock ID; keep them consistent.)*
- **Host-testability (ADR-0001, hold the line).** The RTOS-independent logic — message
  parse/route, the body-control state machine, the `.noinit` handshake encoding — must stay
  host-testable with no FreeRTOS calls in it. It talks to the kernel/CAN through interfaces (a queue
  seam, a CAN HAL) with host fakes. Design that seam deliberately.
- **Memory budget.** CM4 RAM is ~94 KB. Budget FreeRTOS heap + task stacks + app data; prefer static
  allocation for the core tasks; size stacks against the FPU-frame cost.

## Scope for M2 (keep it minimal)

In scope: FreeRTOS up on CM4F (port, config, tick, heap), a minimal real task set, basic CAN RX/TX
(ISR→task), the App-side `.noinit` programming-request **+ pinning `.noinit` in the linkers and ECC
priming** (closes the deferred M1 item), host-testable app logic, and the PendSV/FPU/BASEPRI
explainers as design-essay.

**Deliberately deferred** (do NOT build now):
- Node B actuator app + full inter-node messaging — **later** (M2 is Node A foundation).
- SecOC / crypto on messages — **later** (`shared/secoc`, `shared/crypto` exist as host-testable seams).
- CM0+ security tasks + IPC (hardware semaphores) — **later**.
- UDS / the reprogramming download sequence — **M3** (M2 only *sets* the programming-request).
- App **signature** verification — **M4**.
- Full body-domain feature set — just enough to demonstrate the architecture.

## Deliverables from this session (in order)

1. The **design discussion** above — tradeoffs + alternatives, so I can decide.
2. On agreement: **ADR(s)** — FreeRTOS integration (heap/static, priority model, tick, ISR→task
   pattern); extend **ADR-0007** for the App-side `.noinit` write + the linker `.noinit` pinning;
   a CAN-config ADR if it's non-trivial.
3. The app skeleton: `FreeRTOSConfig.h`, `main()` (`cybsp_init` → create tasks → start scheduler),
   task stubs, and the HAL seams (CAN, `.noinit`) kept host-fakeable.
4. **Failing Unity tests** for the RTOS-independent logic (body-control state machine, message
   routing, `.noinit` handshake encode/decode) with host fakes.
5. Implement against the tests; `make test` + `make lint` green. Target-only port (FreeRTOS port glue,
   CAN driver, `.noinit`/ECC) behind the HAL with host fakes.
6. On-board bring-up: FreeRTOS scheduling, the heartbeat LED driven by a *task*, CAN RX/TX echo, and
   the end-to-end **App-requests-reprogramming** path (App sets `.noinit` → software reset → FBL stays
   in programming mode) — which closes the M1 `.noinit` thread on silicon.

Start with step 1 only. Wait for agreement before writing ADRs or code.

## Watch-fors (call these out, don't gloss)

- **BASEPRI / `configMAX_SYSCALL_INTERRUPT_PRIORITY`** — an ISR calling a `…FromISR` API at too-high
  a priority is the classic, ugly FreeRTOS-on-CM4 fault. Mind `__NVIC_PRIO_BITS`.
- **FPU context switch** — `configENABLE_FPU`/`ARM_CM4F`; PendSV & SVC at lowest priority; lazy-stacking
  vs the FBL handover (FPU state on entry). A wrong PendSV priority "works" until it doesn't.
- **Stack overflow** — `configCHECK_FOR_STACK_OVERFLOW = 2`; the FPU frame makes stacks bigger than reflex.
- **Heap exhaustion / fragmentation** — prefer static allocation for core tasks; if heap_4, watch fragmentation.
- **Handover assumptions** — don't re-enable IRQs early; don't assume the FBL left clocks/peripherals
  configured; `VTOR` is already `app_base`.
- **ADR-0001 leakage** — keep FreeRTOS calls out of the host-testable logic.
- **`.noinit` ECC on first boot** — the priming pattern must handle the first-ever read (the M1 watch-for,
  now actually exercised on silicon).
