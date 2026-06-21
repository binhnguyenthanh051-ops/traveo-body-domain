/*
 * sched_port_arm.c — Cortex-M4F target port for the scheduler (STUB).
 *
 * NOT built for host tests (the host fake in scheduler/tests/sched_port_fake.c
 * provides these symbols there). This file is compiled by ModusToolbox for the
 * CYT2B7 target and is guarded by SCHED_PORT_ARM so the host build skips it.
 *
 * Status: skeleton / TODO until the board arrives. The portable scheduler core
 * is fully host-testable without any of this (ADR-0001). Design: ADR-0009 §D5/§D6.
 */
#if defined(SCHED_PORT_ARM)

#include "sched_port.h"
#include "cmsis_compiler.h"   /* __get_BASEPRI/__set_BASEPRI/__DSB/__ISB, *(verify include)* */

/*
 * Max syscall priority (the BASEPRI ceiling). Interrupts numerically >= this
 * value (lower urgency) are masked inside a critical section; NMI/HardFault and
 * any ISR above the ceiling stay live. Every scheduler-calling ISR (SysTick,
 * later CAN) MUST be configured at a priority >= this; PendSV stays lowest.
 * (ADR-0009 §D6, F4.)
 */
#ifndef SCHED_MAX_SYSCALL_PRIORITY
#define SCHED_MAX_SYSCALL_PRIORITY  0x10U
#endif

/* EXC_RETURN for a fresh task: Thread mode, PSP, standard (non-FP) frame.
 * Bit 4 = 1 → standard frame. A task transitions to the extended (FP) frame
 * automatically on first FPU use. (ADR-0009 §D5, F1.) */
#define SCHED_EXC_RETURN_THREAD_PSP  0xFFFFFFFDU

/*
 * stack_top and arg are intentionally non-const: stack_top points to the
 * writable task stack this function initialises, and arg is forwarded to the
 * task's void * entry. The signature is fixed by sched_port.h and the host
 * fake, so pointer-to-const would be both a prototype mismatch and misleading.
 * cppcheck's constParameterPointer is therefore suppressed per-parameter.
 */
uint32_t *sched_port_init_stack(
    /* cppcheck-suppress constParameterPointer */
    uint32_t       *stack_top,
    sched_task_fn_t entry,
    /* cppcheck-suppress constParameterPointer */
    void           *arg)
{
    /* F5: align the stack top DOWN to an 8-byte boundary (AAPCS / STKALIGN). */
    uintptr_t aligned = (uintptr_t)stack_top & ~((uintptr_t)0x7U);
    uint32_t *sp = (uint32_t *)aligned;

    /* Hardware-unstacked frame {r0-r3, r12, LR, PC, xPSR} */
    *(--sp) = 0x01000000U;                              /* xPSR — Thumb bit */
    *(--sp) = ((uint32_t)(uintptr_t)entry) & 0xFFFFFFFEU; /* PC — F6: mask bit 0 */
    *(--sp) = 0U;                                       /* LR — TODO: task exit hook */
    *(--sp) = 0U;                                       /* r12 */
    *(--sp) = 0U;                                       /* r3  */
    *(--sp) = 0U;                                       /* r2  */
    *(--sp) = 0U;                                       /* r1  */
    *(--sp) = (uint32_t)(uintptr_t)arg;                /* r0 = arg */

    /* Software-saved {r4-r11} + EXC_RETURN (LR value used by the PendSV BX). */
    *(--sp) = SCHED_EXC_RETURN_THREAD_PSP;             /* saved EXC_RETURN */
    for (uint32_t i = 0U; i < 8U; ++i)                 /* r11..r4 */
    {
        *(--sp) = 0U;
    }

    return sp;
}

void sched_port_start_first_task(void)
{
    /* TODO: load PSP from the current TCB->sp, set CONTROL.SPSEL=1 (use PSP in
     * Thread mode), ISB, then branch via EXC_RETURN so the fake frame unwinds
     * into the first task. Does not return. */
}

void sched_port_trigger_switch(void)
{
    /* TODO: pend PendSV — SCB->ICSR = SCB_ICSR_PENDSVSET_Msk. The switch runs
     * when PendSV (lowest priority) is reached. */
}

sched_irq_state_t sched_port_enter_critical(void)
{
    sched_irq_state_t prev = (sched_irq_state_t)__get_BASEPRI();
    /* F4: BASEPRI must be written PRE-SHIFTED into the implemented high bits. */
    __set_BASEPRI(SCHED_MAX_SYSCALL_PRIORITY);  /* TODO: << (8 - __NVIC_PRIO_BITS) */
    __DSB();
    __ISB();
    return prev;
}

void sched_port_exit_critical(sched_irq_state_t prev_state)
{
    __set_BASEPRI((uint32_t)prev_state);   /* F8: restore previous, not 0 */
}

/*
 * PendSV_Handler (naked, target-only) — TODO, see ADR-0009 §D5:
 *   MRS r0, PSP
 *   TST lr, #0x10            ; EXC_RETURN bit 4 == 0 -> FP context active
 *   IT EQ / VSTMDBEQ r0!, {s16-s31}
 *   STMDB r0!, {r4-r11, lr}
 *   ; store r0 -> outgoing TCB->sp ; call core to pick next ; load incoming sp
 *   LDMIA r0!, {r4-r11, lr}
 *   TST lr, #0x10
 *   IT EQ / VLDMIAEQ r0!, {s16-s31}
 *   MSR PSP, r0
 *   BX lr
 */

#endif /* SCHED_PORT_ARM */
