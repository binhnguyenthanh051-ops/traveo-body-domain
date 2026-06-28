# M2 — Node A application on FreeRTOS

> **First deliverable is a design discussion — do NOT let it jump to implementation.**
> Same rhythm as M1: discuss → agree → ADR(s) → failing host tests → implement → on-board
> bring-up. Tradeoffs and at least one alternative per decision (the reasoning is blog material).
>
> *Revised after design review (M2-1…M2-8 folded in): scope is now sequenced, and the
> cross-image / dual-core / handover gaps are made explicit rather than left to be discovered
> on-target.*

## Where this sits

M1 is done and tagged (`m1-bootloader`): the FBL boots → verifies the app (CRC32) → jumps to a
minimal CM4 app at `0x1004_0000`. M2 turns that **blinky stub into the real Node A (gateway)
application running on FreeRTOS** (ADR-0005: the app runs on FreeRTOS; the `scheduler/` module
is a *standalone deep-dive*, NOT the app kernel and NOT linked into any image).

This is also the **interview-prep milestone**: FreeRTOS on Cortex-M4F is where PendSV,
FPU lazy-stacking, and BASEPRI critical sections actually live. The design discussion should
*explain* those internals, not just configure them — that's the point of doing it by hand once.

## Bring up one seam at a time (scope sequencing — M2-1)

M1's lesson: every surprise lived at the hardware seam, and they were tractable because I hit
**one seam at a time**. M2 introduces three new seams (RTOS port, CANFD, ECC-on-`.noinit`) plus
a cross-image integration path. Bring them up in this order, in isolation — do NOT stack them:

1. **FreeRTOS scheduling alone.** Heartbeat LED driven by a *task*, idle task, nothing else.
   Prove the port/tick/priorities before any peripheral is in the picture.
2. **CAN RX/TX echo.** Add CANFD as a second step (ISR→queue→task), once scheduling is solid.
3. **`.noinit`/ECC + App-requests-reprogramming.** Add the shared region, ECC priming, and the
   end-to-end loop last, since it spans both images and is the riskiest interaction.

When something faults, this ordering keeps "which seam?" answerable — same clarity that made M1
debuggable.

## Design discussion (the topics to work through)

- **FreeRTOS port + config.** The `ARM_CM4F` port (hardware FPU). Tick source (SysTick @ a chosen
  `configTICK_RATE_HZ`) on the 160 MHz CM4. **Allocation policy (M2-7):** the principle for a
  body-domain ECU is **no runtime allocation** — static allocation for core tasks
  (`configSUPPORT_STATIC_ALLOCATION`). Don't over-litigate heap_4 vs static; instead decide the
  *architect* question: forbid the heap entirely (`configSUPPORT_DYNAMIC_ALLOCATION = 0`) to make
  "no accidental runtime malloc" a **compile-time** guarantee? If a heap is allowed at all, init-time
  only, never at runtime. `FreeRTOSConfig.h` essentials: priorities, assert hook, stack-overflow
  check, idle/timer tasks.
- **The interrupt-priority model (get this exactly right).** `configMAX_SYSCALL_INTERRUPT_PRIORITY`
  vs `configKERNEL_INTERRUPT_PRIORITY`, **BASEPRI** masking, and this MCU's `__NVIC_PRIO_BITS`. The
  rule any ISR that calls a `…FromISR` API must obey, and the classic fault when it doesn't. (Explain
  it — interview gold.)
- **Context switch internals (design-essay / interview prep).** PendSV as the lowest-priority
  context-switch exception, SVC for the first task, and **FPU lazy stacking** (when the hardware
  actually pushes S0–S31, and why the port reserves room for it). Tie back to what the standalone
  `scheduler/` deep-dive (EP.06-07) does by hand vs how FreeRTOS does it.
- **Exception-state handover from the FBL (M2-2).** This is the first time the *app* does real
  exception-priority work. The FBL used SysTick (for `now_ms`/dwell) and set vectors/priorities for
  *its* world, then quiesced per the B3 contract (IRQs disabled, NVIC cleared, **SysTick stopped +
  pending cleared**, `VTOR = app_base`, `MSP = app SP`). State explicitly: the app **re-initialises
  SysTick and the PendSV/SVC priorities from scratch** (FreeRTOS does this) and must **not** assume any
  FBL tick/SCB config survived. Spell out what the app may and may NOT assume the FBL left.
- **Task / ISR architecture.** Propose a *minimal but real* task set and justify priorities + stack
  sizes — e.g. a **health/heartbeat** task (LED + stack/heap watch; watchdog service later), a **CAN-RX**
  task fed by the CAN ISR via a queue/stream-buffer, and an **app-logic** task (the gateway's actual
  body-domain function). Keep M2 small; design the structure for growth. The ISR→task handoff
  (`xQueueSendFromISR` + `portYIELD_FROM_ISR`) is a first-class topic.
- **The ISR→task test seam (M2-8).** The routing/state-machine logic must consume **decoded messages
  (a plain struct over an interface)**, never a FreeRTOS queue handle directly. The queue is purely
  transport; the host test drives the state machine with structs. Name this seam so the FreeRTOS handle
  cannot leak into host-testable logic (ADR-0001).
- **Watchdog assumption during the FBL→app window (M2-6).** Even though full watchdog integration is
  M6, state the watchdog's **reset-state** now and **who owns it** across the handover: is the WDT/WCO
  counting out of reset (so the app's startup must service/reconfigure it before it bites), or off (so
  the boot-loop fallback is the only net — and it catches *resets*, not *hangs* during app startup)?
  Lock this assumption in the discussion; don't discover it on-target.

### The shared `.noinit` channel (closes the deferred M1 thread) — design carefully

- **Linker pinning, both images + the dual-core RAM tenant (M2-3).** Pin `.noinit` at an **identical
  fixed address** in the FBL and app linker scripts (mismatch ⇒ the handshake reads garbage). The app's
  C-startup must be told **not to zero** it (it is not `.bss`). And — exactly as *flash* had a CM0+
  tenant in M1 — **SRAM is shared between CM0+ and CM4** on this part: place `.noinit` so it avoids the
  **CM0+ RAM region**, not merely the CM4 `.bss`. The memory budget must account for the CM0+ RAM tenant.
- **ECC priming ownership (M2-4).** Lock who primes the region's ECC. Per ADR-0007 D3, the **FBL primes
  on its prime-cause (POR/hibernate/unknown) path**, so by the time the app runs it **consumes a primed,
  ECC-valid region** and never races to prime it. State this so the app is a consumer, not a co-owner —
  and so the FBL never reads the region on a first-ever boot before it's primed.
- **App-side write + the boot-loop-counter interaction (M2-5).** The App writes a programming-request
  into `.noinit` and triggers a **software reset**; the FBL reads it and stays in programming mode. But a
  software reset is exactly what the **M1 boot-loop fallback counts**. The B2 rule — *software reset **with**
  the `.noinit` programming pattern ⇒ clear the counter; **without** ⇒ increment* — is what keeps a
  legitimate reprogram from looking like a crash-loop. M2 is the first silicon exercise of that rule;
  make it an explicit test (below).

- **CAN bring-up.** The gateway needs CAN (the TRAVEO T2G **CANFD** IP — message RAM, RX filters, TX).
  Minimal for M2: one channel, a bitrate, RX→ISR→queue, TX. *(This also unblocks the deferred M1 CAN
  knock window — same peripheral, a fixed knock ID; keep them consistent.)*
- **Host-testability (ADR-0001, hold the line).** The RTOS-independent logic — message
  parse/route, the body-control state machine, the `.noinit` handshake encoding — must stay
  host-testable with no FreeRTOS calls in it. It talks to the kernel/CAN through interfaces (the queue
  seam from M2-8, a CAN HAL) with host fakes. Design that seam deliberately.
- **Memory budget.** CM4 RAM is ~94 KB **after the CM0+ RAM tenant (M2-3)**. Budget FreeRTOS heap (or
  none) + task stacks + app data + the `.noinit` region; prefer static allocation for the core tasks;
  size stacks against the FPU-frame cost.

## Scope for M2 (keep it minimal)

In scope, **in the M2-1 order**: (1) FreeRTOS up on CM4F (port, config, tick, allocation policy) with a
minimal real task set and the heartbeat LED on a task; (2) basic CANFD RX/TX (ISR→task); (3) the App-side
`.noinit` programming-request **+ pinning `.noinit` in both linkers (avoiding the CM0+ RAM tenant) + ECC
priming owned by the FBL** (closes the deferred M1 item). Plus host-testable app logic and the
PendSV/FPU/BASEPRI explainers as design-essay.

**Deliberately deferred** (do NOT build now):
- Node B actuator app + full inter-node messaging — **later** (M2 is Node A foundation).
- SecOC / crypto on messages — **later** (`shared/secoc`, `shared/crypto` exist as host-testable seams).
- CM0+ security tasks + IPC (hardware semaphores) — **later**.
- UDS / the reprogramming download sequence — **M3** (M2 only *sets* the programming-request).
- App **signature** verification — **M4**.
- Full **watchdog** integration — **M6** (M2 only states the reset-state assumption, M2-6).
- Full body-domain feature set — just enough to demonstrate the architecture.

## Deliverables from this session (in order)

1. The **design discussion** above — tradeoffs + alternatives, so I can decide.
2. On agreement: **ADR(s)** — FreeRTOS integration (allocation policy + the `DYNAMIC_ALLOCATION=0`
   decision, priority model, tick, ISR→task pattern); extend **ADR-0007** for the App-side `.noinit`
   write, the linker `.noinit` pinning (incl. the CM0+ RAM tenant), and ECC-priming ownership; a
   CAN-config ADR if non-trivial.
3. The app skeleton: `FreeRTOSConfig.h`, `main()` (`cybsp_init` → create tasks → start scheduler),
   task stubs, and the HAL seams (CAN, `.noinit`) kept host-fakeable.
4. **Failing Unity tests** for the RTOS-independent logic (body-control state machine, message
   routing via decoded structs, `.noinit` handshake encode/decode) with host fakes. **Include the
   M2-5 test:** "App requests reprogram N× in a row ⇒ programming mode every time, boot-loop fallback
   never trips."
5. Implement against the tests; `make test` + `make lint` green. Target-only port (FreeRTOS port glue,
   CANFD driver, `.noinit`/ECC) behind the HAL with host fakes.
6. On-board bring-up **in the M2-1 order**: (1) FreeRTOS scheduling + heartbeat LED on a task;
   (2) CAN RX/TX echo; (3) end-to-end **App-requests-reprogramming** (App sets `.noinit` → software reset
   → FBL stays in programming mode, counter not tripped) — which closes the M1 `.noinit` thread on silicon.

Start with step 1 only. Wait for agreement before writing ADRs or code.

## Watch-fors (call these out, don't gloss)

- **BASEPRI / `configMAX_SYSCALL_INTERRUPT_PRIORITY`** — an ISR calling a `…FromISR` API at too-high
  a priority is the classic, ugly FreeRTOS-on-CM4 fault. Mind `__NVIC_PRIO_BITS`.
- **FPU context switch** — `configENABLE_FPU`/`ARM_CM4F`; PendSV & SVC at lowest priority; lazy-stacking
  vs the FBL handover (FPU state on entry). A wrong PendSV priority "works" until it doesn't.
- **SysTick double-ownership (M2-2)** — FBL used SysTick; the app must re-init it via FreeRTOS and assume
  nothing the FBL left.
- **Dual-core RAM tenant (M2-3)** — `.noinit` (and the whole RAM budget) must dodge the CM0+ SRAM region,
  not just CM4 `.bss`. The M1 flash surprise, repeated in RAM if you don't plan for it.
- **ECC priming ownership (M2-4)** — FBL primes on POR; the app consumes a primed region. The priming
  pattern must handle the first-ever read.
- **Reprogram-request vs boot-loop counter (M2-5)** — the B2 clear-on-pattern rule must fire, or a
  legitimate reprogram looks like a crash-loop. Test it explicitly.
- **Watchdog reset-state (M2-6)** — know whether the WDT is live across the FBL→app window and who services it.
- **Stack overflow** — `configCHECK_FOR_STACK_OVERFLOW = 2`; the FPU frame makes stacks bigger than reflex.
- **Heap exhaustion / fragmentation** — prefer static; consider `DYNAMIC_ALLOCATION = 0` to forbid it outright.
- **Handover assumptions** — don't re-enable IRQs early; don't assume the FBL left clocks/peripherals
  configured; `VTOR` is already `app_base`.
- **ADR-0001 leakage** — keep FreeRTOS calls (and queue handles) out of the host-testable logic.

## Blog note

M2-5 (a legitimate reprogramming request that *looks like* a crash-loop to the boot-loop fallback) is a
strong failure-chapter setup: two individually-correct safety mechanisms that can fight at exactly one
seam. Even if it never misfires on silicon, "here's where I knew they could collide and how I made sure
they didn't" is exactly the architect-judgment narrative the series is for.
