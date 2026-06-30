# ADR-0010: FreeRTOS integration for the Node A application

**Status:** accepted · **Date:** 2026-06-28

## Context

M1 boots the FBL → verifies the app → jumps to a minimal CM4 stub at `0x1004_0000`
(ADR-0008). M2 turns that stub into the real Node A (gateway) application **running on
FreeRTOS** (the standing decision in ADR-0005: the app runs on FreeRTOS; the `scheduler/`
module is a *standalone* deep-dive, not the app kernel).

This ADR fixes the FreeRTOS *integration* decisions: the port, the tick, the allocation
policy, the interrupt-priority model, the task/ISR architecture, the host-test seam, and the
app side of the FBL→app handover contract (the app half of ADR-0008 D4). It targets the
160 MHz Cortex-M4F. The RTOS-independent logic must stay host-testable (ADR-0001) and the
production C is MISRA C:2012 (ADR-0003).

Deliberately **out of scope** (deferred): SecOC/crypto on messages (M4/M5), CM0+ security
tasks + IPC, UDS (M3), full watchdog integration (M6). M2 is the Node A foundation.

## Decisions

### D1. Port = `ARM_CM4F`, FPU on, SysTick tick @ 1000 Hz

- **Port:** `portable/GCC/ARM_CM4F` — the hardware-FPU port. It preserves `S0–S31`/`FPSCR`
  across context switches; the `ARM_CM3` port does not. The FPU is **on**.
- **Tick source:** SysTick, core-local, programmed by the port at `xPortStartScheduler()`.
- **`configTICK_RATE_HZ = 1000`** (1 ms). Overhead is negligible on a 160 MHz core, and 1 ms
  `vTaskDelay` granularity keeps timing code readable. **Tick rate sets `vTaskDelay`
  resolution, not ISR latency** — CAN latency is governed by interrupt priority (D3), not the
  tick.

> Consequence carried into D5: an FPU-using task's full context is `S0–S31 + FPSCR`. Task
> stacks must budget the **FP frame (~136 B)** on top of their logical need.

### D2. Allocation policy — heap **forbidden** (`configSUPPORT_DYNAMIC_ALLOCATION = 0`)

The architect decision for a body-domain ECU: **no runtime allocation, made a compile-time
guarantee.**

- `configSUPPORT_STATIC_ALLOCATION = 1`, `configSUPPORT_DYNAMIC_ALLOCATION = 0`.
- Every kernel object is created with its `…Static` variant; the caller owns the storage
  (`StaticTask_t` + `StackType_t[]` for tasks, `StaticQueue_t` + a byte buffer for queues,
  etc.). Each RTOS allocation becomes a named, reviewable object.
- The app **provides the two kernel static-memory callbacks**, or it will not link:
  `vApplicationGetIdleTaskMemory()` and (because `configUSE_TIMERS = 1`)
  `vApplicationGetTimerTaskMemory()`.

**Why this is stronger than "init-time heap only":** with dynamic allocation compiled out,
any call to a heap-based create API (`xTaskCreate`, `xQueueCreate`, …) **fails to link** —
the symbol does not exist. "No accidental runtime `malloc`" stops being a code-review promise
and becomes a property the linker enforces. It also makes the memory budget *exact* at link
time (no heap to size or fragment).

**Cost accepted:** more static-buffer boilerplate. Mitigated by the buffers being explicit
and auditable — which suits MISRA and the blog ("here is literally every RTOS allocation").

### D3. Interrupt-priority model (get this exactly right)

This MCU uses **`__NVIC_PRIO_BITS = 3`** (8 levels; *verify against the device CMSIS header
when the BSP is vendored*). Priority bits are **left-justified in the 8-bit field** (bits
`[7:5]`), so the raw register values are byte-aligned and everything is written
`level << (8 - __NVIC_PRIO_BITS)` = `level << 5`.

| FreeRTOS config | Value | Meaning |
|---|---|---|
| `configKERNEL_INTERRUPT_PRIORITY` | level 7 → `0xE0` | SysTick + PendSV run here — the *least* urgent exceptions. |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | level 2 → `0x40` | The *most* urgent priority from which a `…FromISR` API may be called. |

The rule, and the classic fault:

- An ISR at level **2–7** (raw `0x40`–`0xE0`) **may** call `…FromISR` APIs.
- An ISR at level **0–1** (raw `0x00`/`0x20`, more urgent than `MAX_SYSCALL`) is
  **kernel-invisible**: it must call **no** FreeRTOS API, but is never delayed by a kernel
  critical section. **M2 defines no level 0–1 ISRs.**
- **BASEPRI, not `cpsid i`.** A FreeRTOS critical section sets `BASEPRI =
  configMAX_SYSCALL_INTERRUPT_PRIORITY` (`0x40`), masking every interrupt of equal-or-lower
  urgency while leaving level 0–1 running with zero added latency. `BASEPRI` is compared
  against the **raw** value — forgetting the `<< 5` shift masks the whole NVIC (the live
  `TODO` in `scheduler/src/sched_port_arm.c:87`, fixed as cleanup so the two ports agree).
- **The default-priority trap:** every IRQ resets to priority **0** (above `MAX_SYSCALL`).
  An IRQ enabled but never assigned a priority sits at level 0; the moment its handler calls
  a `…FromISR` API it can preempt the kernel mid-critical-section and corrupt a kernel list —
  "works under light load, then corrupts under load." **Every kernel-using IRQ has its
  priority set explicitly** to a level numerically ≥ `MAX_SYSCALL` (CAN RX = level 5, D5).
- **`configASSERT` is defined** (trap + record). The port's `vPortValidateInterruptPriority()`
  catches a mis-prioritised `…FromISR` ISR the *first* time — only if asserts are live.

### D4. Context switch + FPU (design-essay; EP.06-07 / interview prep)

Documented as design rationale; the mechanism is the port's, not ours to write:

- **SVC** starts the *first* task once (`svc 0` in the port's start-first-task). **PendSV**
  does *every* subsequent switch. Both sit at **lowest priority** (`0xE0`).
- A tick or `portYIELD()` does not switch inline — it sets the **PendSV pending bit** and
  returns; PendSV, being lowest priority, runs only after every other active/pending ISR
  finishes. This **decouples the decision to switch from the act of switching**, so a switch
  always lands last, in thread-return context, never nested inside another ISR.
- **Two halves of a switch:** hardware auto-stacks the caller-saved frame (`R0–R3, R12, LR,
  PC, xPSR`) on exception entry; the PendSV handler saves callee-saved `R4–R11`, swaps PSP
  via the TCB, picks the next task (`vTaskSwitchContext`), restores, and returns with an
  `EXC_RETURN` that unstacks the new task from **PSP**. Tasks run on **PSP**, handlers on
  **MSP**.
- **FPU lazy stacking:** with `FPCCR` `ASPEN`+`LSPEN` (defaults), exception entry *reserves*
  room for the FP frame but only *writes* `S0–S15`/`FPSCR` if the ISR actually executes an FP
  instruction — fast entry for integer-only ISRs. The `ARM_CM4F` PendSV handler reads
  `EXC_RETURN` bit 4 and additionally saves/restores `S16–S31` for FP-using tasks. Net: an
  FP-using task carries ~136 B more stack (D1/D5). A wrong PendSV priority "works until it
  doesn't" — it can nest a switch into a lazy FP push and corrupt the frame.

### D5. Task & ISR architecture

Five statically-allocated tasks. **Task priority (0 = idle, larger = more urgent) runs
opposite to Cortex-M interrupt priority (0 = most urgent)** — the two namespaces are kept
explicitly separate in code and comments.

| Task | Task prio | Role | Stack (words) |
|---|---|---|---|
| Idle | 0 | FreeRTOS built-in; **idle hook off** (`configUSE_IDLE_HOOK = 0`) | `configMINIMAL_STACK_SIZE` (128) |
| `Health_CyclicTask` | 1 | heartbeat LED + `uxTaskGetStackHighWaterMark` reporting; **M6:** WDT service + per-task check-ins | ~160 |
| `App_CyclicTask` | 2 | gateway body-control state machine (consumes decoded structs) | ~256 |
| `CAN_CyclicTask` | 3 | drains the ISR→task queue, decodes, routes/echoes | ~256 |
| Timer daemon | 4 | FreeRTOS built-in (`configUSE_TIMERS = 1`); software-timer callbacks | `configTIMER_TASK_STACK_DEPTH` (160) |

`configMAX_PRIORITIES = 5`. Rationale: CAN drains above app-logic (deferred-interrupt split,
avoid RX overrun); health lowest of ours (its starvation is itself diagnostic); timer daemon
top so callbacks are timely — **callbacks must be short and non-blocking** (they run in the
daemon context).

**Execution model = "cyclic, hybrid":** each *event-fed* cyclic task blocks on its input queue
*with a timeout equal to its cycle period* — `xQueueReceive(q, &item, pdMS_TO_TICKS(period))` —
so it wakes on an event (low latency) **or** every period (periodic housekeeping + drain). This
honours the cyclic, AUTOSAR-runnable-friendly structure without paying a full period of
latency on every frame. `CAN_CyclicTask` and `App_CyclicTask` are event-fed.
`Health_CyclicTask` is the exception: it has **no input queue**, so it is plain-periodic
(`vTaskDelayUntil`), not the hybrid form.

**Two transport queues** carry the RX path (both static, both in the budget below):
`raw_frame_q` (CAN ISR → `CAN_CyclicTask`, raw frames) and `app_msg_q` (`CAN_CyclicTask` →
`App_CyclicTask`, decoded `body_msg_t`). The decode happens in `CAN_CyclicTask`; only decoded
structs cross into `App_CyclicTask`, which keeps the host-test seam (D6) clean.

**The ISR→task handoff** (first-class):

```c
/* CAN RX ISR — runs at NVIC level 5 (0xA0), kernel-aware. */
BaseType_t woken = pdFALSE;
/* drain the HW RX FIFO into raw_frame_q (see ADR-0011) */
xQueueSendFromISR(raw_frame_q, &frame, &woken);
/* clear the HW RX flag */
portYIELD_FROM_ISR(woken);   /* pend PendSV so CAN_CyclicTask runs on ISR exit */
```

Three invariants: the ISR priority is set explicitly (D3), only `…FromISR` APIs are used, and
`portYIELD_FROM_ISR(woken)` makes the wakeup "interrupt → task" rather than "interrupt → next
tick → task." Transport is a **queue** of fixed-size frames (CAN frames are discrete
messages); a future UART byte-stream will use a **stream buffer** alongside — the choice is
per-peripheral.

### D6. The host-test seam (ADR-0001, M2-8)

The routing / body-control state machine consumes **decoded structs through pure functions**,
never a `QueueHandle_t`:

```c
/* bodyctl.h — no FreeRTOS, no vendor headers */
void bodyctl_init(bodyctl_state_t *st);
void bodyctl_step(bodyctl_state_t *st, const body_msg_t *in, bodyctl_output_t *out);
```

The **enforcement is the build**: `bodyctl.c` and the CAN routing TU must compile on the host
with plain GCC, so a `FreeRTOS.h`/`queue.h` include breaks CI — the leak cannot ship. The
`QueueHandle_t` and the vendor driver live only in the target-only task adapters. Host tests
drive `bodyctl_step()` with hand-built `body_msg_t` structs — no kernel, no kernel-fakes.
Decode uses the existing host-testable `shared/messages` (`unpack_*`). `bodyctl` lives in
`node_a_gateway/app/logic/` with `node_a_gateway/app/logic/tests/` (app-local until a second
consumer justifies `shared/`).

### D7. App side of the FBL→app handover contract (M2-2, M2-6)

The FBL hands over quiescent (ADR-0008 D4, implemented in `port_jump.c`): IRQs disabled +
pending cleared, **SysTick stopped + `PENDSTCLR`/`PENDSVCLR` set**, NVIC banks
disabled/un-pended, `VTOR = app_base`, `MSP` set, `PRIMASK = 1`. The app **re-initialises
everything it uses and assumes nothing the FBL left**:

| App **may** assume | App **must NOT** assume (re-init from scratch) |
|---|---|
| `VTOR` valid — FBL sets it to `app_base`, then Cypress `SystemInit` **relocates it to the RAM vector table** (`.ramVectors`) for runtime ISR registration, so by `main()` it is *not* `app_base` (silicon correction to an earlier draft) | SysTick config — none survives; FreeRTOS reprograms it at scheduler start |
| All peripheral IRQs disabled + un-pended | PendSV/SVC priorities — the port sets them itself |
| No SysTick/PendSV pending | FPU enabled — the port enables `CPACR` CP10/CP11 |
| `MSP` = app initial SP | Any FBL-used peripheral (CAN, LED GPIO) is configured |
| **WDT is OFF** (see below) | Clocks left ready for the app's needs |

- **PRIMASK subtlety:** because this is a *software jump, not a hardware reset*, `PRIMASK = 1`
  carries into the app. The `ARM_CM4F` port clears it (`cpsie i`/`cpsie f`) when it starts the
  first task — so **interrupts come alive exactly at `vTaskStartScheduler()`, not before**.
  No early-init code may block on an interrupt (`cybsp_init` is poll-based — fine).
- **Defensive `PENDSTCLR` re-clear** in the app's early startup (belt-and-suspenders at a
  cross-image seam; one register write that also documents the assumption in code).
- **Trust the contract, verify cheaply:** one or two bring-up asserts (`VTOR == app_base`,
  WDT disabled) as scaffolding, not a runtime dependency.
- **Watchdog reset-state (M2-6 — locked):** the **CM0+ prebuilt disables the WDT**
  (`system_psoc6_cm0plus.c`, `Cy_WDT_Unlock`/`Cy_WDT_Disable`) before the CM4 runs; the CM4
  `SystemInit`'s own WDT-disable is `#if (__CM0P_PRESENT == 0)`-guarded out. So the **WDT is
  OFF across the FBL→app window** and needs no startup servicing. The **only** net against a
  valid-but-misbehaving app in M2 is the BREG boot-loop counter (ADR-0007 D4), which catches
  **resets, not startup hangs**. That gap is bounded and closes in **M6**, when the WDT is
  enabled and serviced by `Health_CyclicTask`. *(Verify on silicon that nothing re-enables
  the WDT.)*

## `FreeRTOSConfig.h` essentials (the locked values)

```c
configCPU_CLOCK_HZ                       160000000
configTICK_RATE_HZ                       1000
configMAX_PRIORITIES                     5
configMINIMAL_STACK_SIZE                 128          /* words */
configSUPPORT_STATIC_ALLOCATION          1
configSUPPORT_DYNAMIC_ALLOCATION         0            /* heap forbidden (D2) */
configUSE_TIMERS                         1
configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 1)   /* 4 */
configTIMER_TASK_STACK_DEPTH             160          /* words — pinned; callbacks stay short */
configTIMER_QUEUE_LENGTH                 8            /* pending timer commands */
configCHECK_FOR_STACK_OVERFLOW           2            /* FP frame makes stacks big (D1/D4) */
configUSE_IDLE_HOOK                      0            /* health is a task, not the hook */
configUSE_MALLOC_FAILED_HOOK             0            /* no heap to fail */
configASSERT(x)                          /* defined: trap + record (D3) */

configPRIO_BITS                          3            /* __NVIC_PRIO_BITS — verify in BSP */
configKERNEL_INTERRUPT_PRIORITY          (7 << (8 - configPRIO_BITS))   /* 0xE0 */
configMAX_SYSCALL_INTERRUPT_PRIORITY     (2 << (8 - configPRIO_BITS))   /* 0x40 */
```

## Host/target split (ADR-0001)

| Host-testable | Target-only |
|---|---|
| `bodyctl` state machine (D6); message routing | FreeRTOS port glue (PendSV/SVC/SysTick) |
| `shared/messages` decode (existing) | The task adapters that own the `QueueHandle_t`s |
| CAN frame routing over `can_raw_frame_t` structs | CAN driver + ISR (ADR-0011); LED/button GPIO |
| App-side `.noinit` encode (ADR-0007 ext.) | `app_port_*` (noinit region, system reset) |

## Consequences

- (+) No-runtime-allocation is a **link-time** guarantee; the memory budget is exact.
- (+) The handover is a written contract the app satisfies by design, not by luck.
- (+) The state machine is the most testable part of the system — structs in, outputs out,
  no kernel — and the FreeRTOS handle cannot leak into it.
- (+) The interrupt-priority model is stated once, with the classic fault called out.
- (−) Static allocation adds buffer boilerplate (accepted; it is auditable).
- (−) `__NVIC_PRIO_BITS`, FPU-frame sizing, and the WDT-off assumption ride on
  TRM/BSP specifics confirmed on silicon (below).

## Alternatives considered

- **`heap_4`, init-time only:** ergonomic `xTaskCreate`, coalesced startup allocation — but
  the guarantee degrades from compile-time to review-enforced. Rejected for a safety-adjacent
  ECU; the static boilerplate is the price of the stronger claim.
- **`ARM_CM3` port, FPU off:** smaller, uniform context frames — but throws away the M4F and
  blocks single-precision float in app logic. Rejected.
- **Merge `App_CyclicTask` + `CAN_CyclicTask`:** fewer tasks, but loses the deferred-interrupt
  split and muddies the host-test seam. Rejected; kept split.
- **Health in the idle hook:** gives a free "CPU-not-starved" signal, but cannot do per-task
  check-ins for the M6 watchdog. Rejected in favour of a dedicated low-priority health task.
- **Tick @ 100 Hz:** fewer tick ISRs, but coarse `vTaskDelay`; the 1000 Hz overhead is noise
  on this core. Rejected.

## To verify in the TRM / BSP / on silicon

- `__NVIC_PRIO_BITS` (assumed 3) against the vendored CMSIS device header.
- FPU-frame stack costs trimmed from `uxTaskGetStackHighWaterMark` during bring-up.
- Nothing re-enables the WDT across the FBL→app window (M2-6).
- The port's SysTick reprogramming vs the FBL's stop is conflict-free (M2-2) on board.

## Review history

Design-reviewed (`docs/review/ADR-0010-0011-review.md`) before implementation, then corrected
against silicon during M2 bring-up. Findings actioned here: **1** — named `app_msg_q` (the
CAN→App decoded-struct queue) and added it to the budget (D5); **3** — clarified
`Health_CyclicTask` is plain-periodic, not the hybrid cyclic form (D5); **5** — pinned
`configTIMER_TASK_STACK_DEPTH` / `configTIMER_QUEUE_LENGTH`; **S1** (silicon) — corrected the
handover claim: Cypress `SystemInit` relocates `VTOR` to the RAM vector table, so it is *not*
`app_base` by `main()` (D7).
