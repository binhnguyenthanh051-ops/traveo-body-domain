# ADR-0009: Scheduler design — fixed-priority preemptive with portable core

**Status:** accepted · **Date:** 2026-06-20

## Context

The custom scheduler (ADR-0005) is a standalone deep-dive, not the product kernel. Its
purpose is to demonstrate how a preemptive RTOS works from scratch, anchor the EP.14
FreeRTOS comparison, and serve as the technical depth piece of the portfolio. The design
must be host-testable (ADR-0001), MISRA-clean (ADR-0003), and clearly separable into
*portable core logic* and *target-specific port code*.

This ADR records the seven key design decisions and the rationale behind each. Where the
design diverges from FreeRTOS, the difference is noted explicitly — the comparison is a
first-class deliverable.

---

## Decisions

### D1. Scheduling policy — fixed-priority preemptive with tick-based round-robin

**Decision:** strictly preemptive by priority. When multiple tasks share a priority, the
tick ISR rotates the running task to the back of that priority's ready list (cooperative
round-robin among equals).

**Rationale:** this mirrors the OSEK/AUTOSAR BCC2/ECC2 model and is the automotive-
relevant choice. Pure strict-priority (no rotation) risks starvation among equal-priority
tasks. Full per-task quantum tracking is unnecessary for the small task set in scope and
adds bookkeeping with no architectural insight.

**FreeRTOS comparison:** FreeRTOS implements the same policy when
`configUSE_TIME_SLICING = 1` (the default). The difference is cosmetic: FreeRTOS tracks
an explicit "ticks since last switch" counter per priority; we rotate unconditionally on
each tick, which is equivalent when the quantum is one tick.

**Alternative rejected — cooperative only:** dramatically simpler (no involuntary
preemption), but loses the preemptive story and is not the automotive model.

### D2. TCB and static task allocation

**Decision:** the task control block (TCB) contains:

| Field          | Type               | Purpose                                     |
|----------------|--------------------|---------------------------------------------|
| `sp`           | `uint32_t *`       | saved stack pointer (PSP on target)          |
| `priority`     | `uint8_t`          | 0 = highest                                 |
| `state`        | `sched_task_state_t` | READY / RUNNING / BLOCKED / SUSPENDED      |
| `delay_ticks`  | `uint32_t`         | remaining ticks for `sched_delay()`; 0 = n/a|
| `stack_base`   | `uint32_t *`       | bottom of the statically allocated stack     |
| `stack_size`   | `uint32_t`         | in 32-bit words                             |
| `entry`        | `sched_task_fn_t`  | task entry point                             |
| `id`           | `uint8_t`          | index in the task table                      |

All TCBs and stacks come from statically sized arrays (`SCHED_MAX_TASKS`,
`SCHED_STACK_WORDS_*`). No `malloc`.

On task creation the port's `sched_port_init_stack()` writes a **fake exception frame**
so the first context restore "returns" into the task's entry function. The frame includes
a deliberate `task_exit_hook` in the LR slot — if a task returns from its entry function,
this traps the error rather than jumping to garbage.

**FreeRTOS comparison:** FreeRTOS *defaults* to `pvPortMalloc` for TCBs and stacks but
supports static allocation via `xTaskCreateStatic`. Our design enforces static allocation
unconditionally — appropriate for MISRA/ASIL-aware code and for a system with only 128 KB
SRAM.

**Alternative rejected — heap allocation:** violates ADR-0003 (MISRA: no dynamic memory).

### D3. Ready-set — priority bitmap with `__builtin_ctz`

**Decision:** a `uint32_t ready_bitmap` where bit *N* is set when at least one task at
priority *N* is ready. The highest-priority ready level is found with
`__builtin_ctz(ready_bitmap)` — a single machine instruction on both ARM (`RBIT+CLZ`)
and x86 (`BSF`/`TZCNT`). Each priority level maintains a small ready list for round-robin
rotation.

**Host-testability:** `__builtin_ctz` is a GCC/Clang built-in that compiles on any target
those compilers support. No HAL seam is needed — it is a compiler intrinsic, not a
hardware peripheral. If portability beyond GCC is ever required, a 4-iteration binary-
search fallback is trivial and still O(1).

**The ready set is never empty — built-in idle task (F2).** `__builtin_ctz(0)` is undefined
behaviour (on ARMv7-M it computes 32 — an out-of-range priority index, not a usable
result), so the selector must never see an empty `ready_bitmap`. The scheduler therefore
creates a built-in **idle task** at the lowest priority (`SCHED_MAX_PRIORITIES - 1`), always
READY, running a `WFI` loop on target. It is created automatically by `sched_init()` (it
does not consume a user `SCHED_MAX_TASKS` slot — it lives in a reserved slot), so the ready
set is non-empty *by construction*: the UB is gone, `sched_start()` with no user tasks is
well-defined, and idle is the natural home for low-power `WFI`. The core selector still
guards `ready_bitmap == 0` defensively, but with idle present that branch is unreachable in
normal operation. FreeRTOS creates exactly this idle task, for exactly this reason.

**FreeRTOS comparison:** FreeRTOS uses the same bitmap approach
(`uxTopReadyPriority` + `portGET_HIGHEST_PRIORITY` macro wrapping CLZ). Our
implementation is structurally identical, which makes the EP.14 comparison about framing
rather than mechanism.

**Alternative rejected — linear scan:** for 4–8 tasks the performance is the same, but
the bitmap is the canonical RTOS data structure and demonstrates the O(1) property
clearly.

### D4. Port interface — five functions, link-time selection

**Decision:** the scheduler core depends on a minimal port interface (`sched_port.h`):

```c
uint32_t          *sched_port_init_stack(uint32_t *stack_top,
                                         sched_task_fn_t entry, void *arg);
void               sched_port_start_first_task(void);
void               sched_port_trigger_switch(void);
sched_irq_state_t  sched_port_enter_critical(void);          /* returns prev BASEPRI */
void               sched_port_exit_critical(sched_irq_state_t prev_state);
```

`sched_irq_state_t` is `uint32_t` (the BASEPRI value). `enter` returns the previous mask so
`exit` can restore it (F8); the core holds that value in a local across the matched pair.

The target port and the host fake each implement these five functions; the linker selects
which object file to include. No vtable, no function-pointer struct.

A sixth function, `sched_tick()`, is *core-side* (not part of the port). The target
port's SysTick handler calls it; host tests call it directly.

**FreeRTOS comparison:** FreeRTOS uses a similar but larger port interface
(`port.c` / `portmacro.h`), with ~15 macros/functions per port. Ours is deliberately
smaller because we have fewer features — we add to it only when a feature demands it.

**Alternative rejected — vtable (function-pointer struct):** adds runtime indirection
for no benefit — the scheduler port is a singleton; you never swap it at runtime. The
existing `hal.h` uses vtables for drivers that could have multiple instances (flash,
crypto); the scheduler doesn't share that property.

**Alternative rejected — merge into `hal.h`:** the scheduler is standalone (ADR-0005)
and not linked into product images. Keeping its port header in `scheduler/include/`
avoids coupling product code to scheduler internals.

### D5. Context switch — PendSV + SysTick, PSP/MSP split, M4F FPU handling

**Decision:** the canonical Cortex-M approach:

- **Tasks** run in Thread mode using **PSP**.
- **Kernel + handlers** use **MSP**.
- **PendSV** at the lowest exception priority performs the context switch.
- **SysTick** at a higher priority calls `sched_tick()`, which pends PendSV if preemption
  is needed. PendSV tail-chains after SysTick returns.

The PendSV handler:

1. On entry, hardware has auto-stacked `{r0–r3, r12, LR, PC, xPSR}` from the outgoing
   task's PSP.
2. Software saves `{r4–r11, EXC_RETURN}` manually.
3. If EXC_RETURN bit 4 = 0 (FP context active), software saves `{s16–s31}`.
4. Stores the updated PSP into the outgoing TCB.
5. Calls the core to select the next task; loads the incoming TCB's SP.
6. Restores `{s16–s31}` if EXC_RETURN bit 4 = 0, then `{r4–r11, EXC_RETURN}`.
7. Sets PSP to the incoming stack; `BX LR` triggers exception return, which auto-pops
   `{r0–r3, r12, LR, PC, xPSR}`.

**M4F FPU — lazy stacking:**

The Cortex-M4F's lazy stacking (`FPCCR.LSPEN`, enabled by default) means:

- On exception entry with FPU active, hardware *reserves* space for `{s0–s15, FPSCR}` on
  the stack but **does not write** them unless the handler touches the FPU.
- EXC_RETURN bit 4 indicates whether the stacked frame is extended (FP) or standard.
- The PendSV handler must save/restore **s16–s31** (callee-saved FP regs) — hardware
  manages s0–s15 via lazy stacking.
- EXC_RETURN is saved alongside r4–r11 because each task may have a different value.

**Initial stack frame:** `sched_port_init_stack()` sets `EXC_RETURN = 0xFFFFFFFD`
(Thread mode, PSP, **standard / non-FP frame** — bit 4 = 1) and writes the Thumb-bit-set
xPSR (`0x01000000`). A freshly created task has not used the FPU, so it correctly starts
with the standard frame and the conditional `{s16–s31}` save in steps 3/6 is skipped for
it. The task transitions to the extended (FP) frame *automatically* the first time it
executes an FPU instruction: hardware sets `CONTROL.FPCA`, the next exception entry builds
the extended frame, and the per-task saved `EXC_RETURN` then carries bit 4 = 0 so the
conditional save engages from that point on. This is exactly the FreeRTOS behaviour.

> **Review note (F1):** an earlier draft labelled `0xFFFFFFFD` the "extended frame" and
> claimed all tasks start extended (18 extra words each). That was wrong on two counts —
> bit 4 = 1 is the *standard* frame, and starting every task extended would make the
> conditional FP-save optimisation dead code. The value `0xFFFFFFFD` was always correct;
> only the prose described a worse, contradictory design. The extended-frame `EXC_RETURN`
> value, for reference, is `0xFFFFFFED` (bit 4 = 0).

**FreeRTOS comparison:** FreeRTOS's Cortex-M4F port uses the same PendSV/lazy-stacking
mechanism. The primary differences: (a) FreeRTOS wraps the PendSV in more macros for
portability across 40+ architectures; (b) FreeRTOS has a `configENABLE_FPU` switch that
can omit FP save/restore entirely. We always handle FP for simplicity and explanation
value.

**Alternative rejected — disable lazy stacking:** forces eager save of s0–s15 on every
exception entry, increasing worst-case ISR latency for no benefit.

**Alternative rejected — unconditional s16–s31 save:** simpler but wastes ~18 cycles per
switch for non-FP tasks. The `TST lr, #0x10` branch is two instructions and worth the
explanation.

**Target-port footguns (F5/F6 — apply when `sched_port_init_stack` is written for ARM):**

- **8-byte stack alignment.** AAPCS and the Cortex-M `STKALIGN` behaviour require the stack
  to be 8-byte aligned at exception entry. The port must align the stack top down to an
  8-byte boundary *before* writing the fake frame, or rare alignment-dependent faults appear
  that look random.
- **Mask bit 0 of the stacked PC.** Function pointers in Thumb state carry bit 0 = 1. The
  Thumb state on exception return comes from `xPSR.T` (already set via `0x01000000`), so the
  PC slot must be written as `entry & 0xFFFFFFFEu`. FreeRTOS does the same with a
  `portSTART_ADDRESS_MASK`.

These are target-only and not observable on the host fake; they are recorded here so the
ARM port carries them from day one rather than rediscovering them on hardware.

### D6. Critical sections — BASEPRI with configurable ceiling

**Decision:** critical sections raise `BASEPRI` to `SCHED_MAX_SYSCALL_PRIORITY`, masking
interrupts at or below that level. Interrupts above the ceiling (NMI, HardFault,
safety-critical watchdog) remain live. Enter **saves and restores the previous `BASEPRI`**
rather than forcing it to 0 on exit, so the pair composes correctly even if it is ever
entered with `BASEPRI` already raised (e.g. from an ISR):

```
enter_critical:
    uint32_t prev = __get_BASEPRI();
    __set_BASEPRI(SCHED_MAX_SYSCALL_PRIORITY << (8U - __NVIC_PRIO_BITS));
    __DSB(); __ISB();
    return prev;                  /* caller holds prev; restored on exit */
exit_critical(prev):
    __set_BASEPRI(prev);
```

**BASEPRI must be written pre-shifted (F4).** Cortex-M implements only the top
`__NVIC_PRIO_BITS` priority bits, so the ceiling is shifted left by `(8 - __NVIC_PRIO_BITS)`
before it goes into `BASEPRI`. Writing the raw priority number silently mis-masks.

**Platform constraint — ISR priorities must respect the ceiling (F4 corollary).** Every
interrupt that calls into the scheduler (SysTick now; CAN, timers later) must be configured
at a priority numerically **≥** `SCHED_MAX_SYSCALL_PRIORITY` (i.e. *lower* urgency), so a
critical section actually masks it. **PendSV stays at the lowest priority of all.** ISRs
above the ceiling must not call scheduler APIs.

The host fake increments/decrements a nesting counter, letting tests verify that critical
sections are balanced.

**FreeRTOS comparison:** FreeRTOS uses the same BASEPRI approach
(`configMAX_SYSCALL_INTERRUPT_PRIORITY`). One honest distinction worth noting (F8): FreeRTOS
*task-level* `taskENTER_CRITICAL` uses a **nesting counter** and restores `BASEPRI` to 0
only at depth 0, because task code is never entered with `BASEPRI` already raised; its
**`..._FROM_ISR`** variant uses the save/restore-previous pattern precisely because an ISR
might already be above the ceiling. We adopt the save/restore-previous form *everywhere* for
uniformity — it is strictly the more general of the two, handles nesting implicitly with no
separate counter, and removes a class of future bugs at the cost of returning one value from
`enter`. This is the only place our critical-section design intentionally differs from the
FreeRTOS task-level path, and the EP.14 write-up should call it out.

**Alternative rejected — PRIMASK (`CPSID i` / `CPSIE i`):** masks *all* interrupts
except NMI and HardFault. Acceptable for trivial systems, but on a system claiming any
safety awareness, blanket-masking ISRs is a red flag. BASEPRI is the right tool.

### D7. Tick and delay — global counter with per-task countdown

**Decision:** SysTick calls `sched_tick()`, which:

1. Increments a global `g_tick_count`.
2. Walks the task table: for each `BLOCKED` task with `delay_ticks > 0`, decrements it.
   If the countdown reaches zero, the task moves to `READY` and its priority bit is set
   in the bitmap.
3. Pends PendSV if **either** (a) an unblocked task is higher priority than the running
   task, **or** (b) more than one task is ready at the *current* running priority — in which
   case it also advances that level's round-robin index so a *different* equal-priority task
   is selected next.

> **Review note (F3):** rotating the equal-priority ready list without pending PendSV is a
> silent no-op — the rotated-in task never actually runs until some unrelated switch occurs.
> Equal-priority time-slicing (D1) therefore only works because step 3(b) pends PendSV on
> rotation, not just on a higher-priority wake. The implementation does this; an earlier
> draft of this pseudo-code only listed the higher-priority case.

Public API: `sched_delay(uint32_t ticks)` — blocks the calling task for *ticks* ticks.
**`sched_delay(0)` is defined as equivalent to `sched_yield()`** (F9): the task is not
blocked, but the scheduler advances the round-robin index at its priority and re-selects,
giving an equal-priority peer a turn. This is documented in the `sched.h` contract.

**Host-testability:** tests call `sched_tick()` directly N times and assert on task
states and the ready bitmap. No real timer is involved.

**FreeRTOS comparison:**

| Aspect             | Our scheduler            | FreeRTOS                          |
|--------------------|--------------------------|-----------------------------------|
| Delay bookkeeping  | per-task countdown       | sorted delta delay list           |
| Tick cost           | O(N) — scan all tasks    | O(1) — check list head only       |
| Insertion cost      | O(1)                     | O(N) — insert into sorted list    |
| Tick wrap           | not an issue (countdown) | handled (two delay lists, toggled)|
| Tickless idle       | not implemented          | `vPortSuppressTicksAndSleep()`    |

For 4–8 tasks the O(N) tick scan is trivially fast (single-digit microseconds). The delta
list is the right optimisation at scale and is a planned EP.14 discussion point.

**Alternative rejected — absolute wake-up times:** avoids the per-tick decrement loop
but needs 32-bit overflow handling (wrap at ~49 days at 1 kHz). Both approaches work;
the countdown is more intuitive to explain and test.

**Alternative deferred — tickless idle:** out of scope per the brief. Good EP.14
comparison topic (FreeRTOS supports it; ours does not).

---

## Deliberately deferred

These are held back for later episodes — do NOT add them now:

- **Mutexes / priority inheritance** — EP.08 (priority-inversion failure chapter).
- **M0+ / inter-core IPC** — EP.11.
- **Per-task MPU isolation** — future enhancement.
- **Tickless idle** — EP.14 comparison topic.
- **FreeRTOS head-to-head comparison** — EP.14.

## Consequences

- (+) Clean core/port separation: the decision logic is fully host-testable under CI.
- (+) Every mechanism has a named FreeRTOS counterpart, enabling a concrete comparison.
- (+) Static allocation and BASEPRI are the MISRA/ASIL-aligned choices; defensible in an
  interview.
- (+) FPU lazy stacking is explicitly handled, not glossed — the M4F gotcha is addressed.
- (+) A built-in idle task keeps the ready set non-empty by construction, eliminating the
  `__builtin_ctz(0)` UB and making `sched_start()` total (F2).
- (+) Critical sections save/restore the previous `BASEPRI`, so they compose under nesting
  and from raised contexts (F8).
- (−) The O(N) tick scan does not scale to hundreds of tasks (irrelevant at this scope).
- (−) The scheduler is not used in production — framed honestly as a deep-dive (ADR-0005).

## Review history

ADR-0009 was design-reviewed before implementation (see `docs/review/ADR-0009-review.md`).
Findings actioned: F1 (EXC_RETURN relabel, D5), F2 (idle task, D3), F3 (pend PendSV on
equal-priority rotation, D7), F4 (pre-shifted BASEPRI + ISR-priority constraint, D6), F5/F6
(stack alignment + PC bit-0 mask recorded as target-port footguns, D5), F7 (MISRA deviations
for `__builtin_ctz` and port asm — see `docs/coding-standard.md`), F8 (save/restore previous
BASEPRI, D6), F9 (`sched_delay(0)` defined as yield, D7). F1 and F3 were both
"correct-on-paper, silently-wrong-in-behaviour" — caught in review rather than on hardware,
which is the argument for design-first.
