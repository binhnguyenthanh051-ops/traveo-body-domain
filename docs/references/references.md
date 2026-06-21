# Further reading — RTOS internals & Cortex-M

Curated study material for the scheduler design (ADR-0009) and its review
(`docs/reviews/ADR-0009-review.md`). Mapped to the specific decisions/findings so you read
the right thing for each. Seeds the "further reading" section of the S.1/S.2 blog posts.

**Suggested order:** Memfault context-switching article → Yiu's exception/context chapters
(or Samek's videos) → ARM AN298 for the FPU detail → FreeRTOS `port.c` + the Mastering book
for the comparison.

## Start here — connects everything
- **Memfault Interrupt — "ARM Cortex-M RTOS Context Switching"** —
  https://interrupt.memfault.com/blog/cortex-m-rtos-context-switching
  Walks `xPortPendSVHandler` step-by-step with gdb; covers the **fake initial stack frame**
  (D2), the context switch + FPU (D5), and "how does the scheduler start at boot?". Its
  comments also flag the conditional-FPU-stacking subtlety — corroborates **F1**.

## Cortex-M fundamentals (modes, MSP/PSP, exceptions, NVIC) — D5/D6
- **Memfault — "A Practical Guide to ARM Cortex-M Exception Handling"** —
  https://interrupt.memfault.com/blog/arm-cortex-m-exceptions-and-nvic
- **Joseph Yiu — "The Definitive Guide to ARM Cortex-M3 and Cortex-M4 Processors"** (book) —
  canonical; context-switch section shows the PendSV R4–R11 save/restore split from first
  principles.
- **ARMv7-M Architecture Reference Manual** + **Cortex-M4 Devices Generic User Guide**
  (developer.arm.com) — authoritative for EXC_RETURN values, BASEPRI, FPU regs. Use to
  settle F1 definitively.

## FPU / lazy stacking — F1, D5 (the trickiest bit)
- **ARM AN298 — "Cortex-M4(F) Lazy Stacking and Context Switching"** —
  https://developer.arm.com/documentation/dai0298/a/
  (mirror: https://www.state-machine.com/doc/ARM-AN298.pdf)
  THE primary source on lazy stacking, FPCA, LSPACT, and EXC_RETURN bit 4.
  **Caveat:** AN298's *example assembly* has a known FP-context bug (ChibiOS / Andrea Biondo
  analysis). Read it for the mechanism; do NOT copy its sample code verbatim. This is exactly
  why F1 matters.

## Critical sections & interrupt priorities — F4, D6
- **MCU on Eclipse — "ARM Cortex-M Interrupts and FreeRTOS" (Parts 1–3)** —
  https://mcuoneclipse.com/2016/08/28/arm-cortex-m-interrupts-and-freertos-part-3/
  Explains `configMAX_SYSCALL_INTERRUPT_PRIORITY`, PSP-for-tasks/MSP-for-ISRs, and the
  inverted numbering (FreeRTOS 0 = lowest urgency vs NVIC 0 = highest) — the root of the
  BASEPRI pre-shift footgun.

## The production RTOS you compare against — S.4 / EP.14
- **"Mastering the FreeRTOS Real Time Kernel"** (official, free) —
  https://github.com/FreeRTOS/FreeRTOS-Kernel-Book (PDF under Releases).
  Best text for task states, delays (the delta-list in D7), scheduling.
- **FreeRTOS-Kernel source** — https://github.com/FreeRTOS/FreeRTOS-Kernel
  Read `portable/GCC/ARM_CM4F/port.c` (real PendSV handler) next to the Memfault article;
  `tasks.c` for the ready-list bitmap (`uxTopReadyPriority`) behind D3.
- **FreeRTOS docs hub** — https://www.freertos.org/Documentation/00-Overview

## Build a kernel from scratch (fundamentals, video)
- **Miro Samek — "Modern Embedded Systems Programming Course"** (free, YouTube +
  state-machine.com). Later lessons build a preemptive Cortex-M kernel from scratch
  including PendSV + FPU. Best paced walk-through if the blogs assume too much.

---
*Books are cited by title (no link). All URLs verified 2026-06-20; if one moves, search the
title.*
