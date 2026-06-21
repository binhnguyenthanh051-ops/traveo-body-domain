# Design brief — EP.06–07: custom preemptive scheduler

> **How to use this:** paste this as your opening message to Claude Code in the repo
> root. It already has `CLAUDE.md`; this brief narrows the task. Do NOT let it jump
> straight to implementation — the first deliverable is a design discussion.

## Goal

Build a minimal **fixed-priority preemptive scheduler** that runs a handful of tasks on
the **Cortex-M4**, as a **standalone deep-dive** (ADR-0005) — it is NOT the product app
kernel (the app runs on FreeRTOS) and is not linked into any image. It runs a representative
task set on its own, anchors the EP.14 FreeRTOS comparison, and is the technical depth piece
of the portfolio, so design and tradeoffs matter as much as working code. The M0+ is out of
scope here. One core, done well.

## Hard constraints (non-negotiable)

1. **Host-testability (ADR-0001).** The scheduler *decision logic* — task table, ready
   set, priority selection, task-state transitions, tick/delay bookkeeping — must compile
   and unit-test on x86 with GCC. The actual context switch (register save/restore, PSP
   manipulation, SysTick wiring, starting the first task) is target-only and lives behind
   a **port interface** in `common/hal`, with a host fake for tests.
   - The seam: the **core decides** "next task = T"; the **port performs** the switch.
     That separation is the whole point — design the interface around it.
2. **MISRA C:2012 (ADR-0003).** Static allocation only — no malloc; tasks and stacks come
   from statically-sized pools. Fixed-width types. No `<stdio.h>` in production code.
3. **Comprehension gate (docs/workflow.md).** I must be able to explain every line in an
   interview, especially the context-switch path. Explain as you go; don't hand me a
   working black box.

## What I want you to PROPOSE first (before any code)

Give me a short design discussion with tradeoffs and at least one alternative for each:

- **Scheduling policy.** Fixed-priority preemptive is the target (it mirrors OSEK/AUTOSAR
  OS and is the automotive-relevant choice). Propose how equal-priority tasks are handled
  (strict priority? round-robin among equals via the tick?) and justify it.
- **TCB design.** What's in the task control block; how task stacks are statically
  allocated and how a new task's **initial stack frame is faked** so the first context
  restore runs it cleanly.
- **Ready-set / next-task selection.** Data structure and selection algorithm. If you
  propose a priority bitmap with CLZ, show how the *core logic stays portable/host-testable*
  (CLZ is a target intrinsic) — e.g. a portable fallback behind the same interface.
- **The port interface (the HAL seam).** Exactly what functions the core needs from the
  port: trigger a context switch, start the first task, init a task stack, enter/exit
  critical section. Keep it minimal and host-fakeable.
- **Context-switch mechanism (target side).** Confirm the canonical Cortex-M approach and
  explain it: PendSV at lowest exception priority for the switch, SysTick for the tick,
  **PSP for tasks / MSP for the kernel+handlers**, hardware auto-stacking of
  {r0–r3, r12, LR, PC, xPSR} vs software saving of {r4–r11}. **Address the M4F FPU**:
  lazy stacking and how the FP context {s16–s31} is handled via EXC_RETURN — this is the
  gotcha I specifically want covered, not glossed.
- **Critical sections.** BASEPRI vs PRIMASK, and why — including a configurable
  max-syscall priority so high-priority/fault handlers stay live.
- **Tick & delays.** How the tick drives time-based unblocking (a task `sleep`/delay),
  kept in host-testable bookkeeping.

## Scope for EP.06–07 (keep it minimal)

In scope: static task creation, start scheduler, preemptive switch on tick and on explicit
yield, a blocking delay/sleep, fixed priorities.

**Deliberately deferred** (do NOT add these now):
- Mutexes / priority inheritance — held back so **priority inversion can be the EP.08
  failure chapter**. Don't pre-solve it.
- M0+ / inter-core IPC (EP.11), per-task MPU isolation, tickless idle, FreeRTOS
  comparison (EP.14).

## Deliverables from this session (in order)

1. The design discussion above — tradeoffs + alternatives, so I can decide.
2. Once I agree: **ADR-0004** (`docs/architecture/decisions/`) recording the scheduler
   design and the decisions made.
3. The public interface in `scheduler/include/` (header only, documented).
4. A **failing Unity test** for the next-task selection + task-state logic. This also
   drives migrating the host harness in `shared/messages/tests/` to Unity
   (ThrowTheSwitch) — set that up as part of this.
5. Then implement the core against the test + a host fake port; get `make test` and
   `make lint` green. The target-side PendSV/SysTick port can be stubbed/marked TODO
   until the board arrives — the core logic should be fully testable without hardware.

Start with step 1 only. Wait for my agreement before writing the ADR or any code.
