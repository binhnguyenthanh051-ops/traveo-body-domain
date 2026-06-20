/*
 * sched_port.h — port interface (the HAL seam).
 *
 * The scheduler core calls these five functions; the linker selects either
 * the target implementation (PendSV/SysTick/BASEPRI) or the host fake.
 * See ADR-0009 §D4 for rationale.
 *
 * This header lives in scheduler/include/, NOT in shared/hal/, because the
 * scheduler is a standalone module (ADR-0005) not linked into product images.
 */
#ifndef SCHED_PORT_H
#define SCHED_PORT_H

#include "sched_types.h"

/*
 * Saved interrupt-mask state returned by enter_critical and consumed by
 * exit_critical. On target this is the previous BASEPRI value; on host it is
 * the previous emulated mask. (ADR-0009 §D6, F8: save/restore previous.)
 */
typedef uint32_t sched_irq_state_t;

/*
 * Build the initial (fake) exception stack frame for a new task so that the
 * first context restore "returns" into entry(arg).
 *
 * stack_top points one word past the last usable stack word (grows down).
 * Returns the resulting stack pointer (to be stored in tcb->sp).
 *
 * Target: writes xPSR (Thumb bit), PC=entry, LR=exit_hook, r0=arg, plus
 *         space for the software-saved r4–r11 and EXC_RETURN. Two target-only
 *         requirements the host fake need not honour (ADR-0009 §D5, F5/F6):
 *           - align stack_top DOWN to an 8-byte boundary before writing the
 *             frame (AAPCS / STKALIGN), else rare alignment faults appear;
 *           - mask bit 0 of the entry address in the stacked PC slot
 *             (PC = entry & 0xFFFFFFFEu) — Thumb state comes from xPSR.T.
 * Host:   writes a simplified frame the test harness can inspect.
 */
uint32_t *sched_port_init_stack(uint32_t       *stack_top,
                                sched_task_fn_t entry,
                                void           *arg);

/*
 * Start the very first task — sets PSP, drops to Thread mode, never returns.
 *
 * Target: loads PSP from sp, sets CONTROL.SPSEL, branches via EXC_RETURN.
 * Host:   calls the task's entry function directly.
 */
void sched_port_start_first_task(void);

/*
 * Request a context switch.
 *
 * Target: pends PendSV (ICSR |= PENDSVSET). The actual switch happens when
 *         PendSV runs at the lowest exception priority.
 * Host:   sets a flag or calls the core's switch logic synchronously.
 */
void sched_port_trigger_switch(void);

/*
 * Enter a scheduler critical section — mask interrupts up to the syscall
 * ceiling and RETURN the previous mask so exit can restore it (F8). The core
 * holds the returned value in a local across the matched enter/exit pair.
 *
 * Target: prev = __get_BASEPRI();
 *         __set_BASEPRI(SCHED_MAX_SYSCALL_PRIORITY << (8 - __NVIC_PRIO_BITS));
 *         DSB; ISB; return prev.
 * Host:   saves+raises an emulated mask, bumps a nesting counter (tests assert
 *         it stays balanced), returns the previous emulated mask.
 */
sched_irq_state_t sched_port_enter_critical(void);

/*
 * Exit a scheduler critical section — restore the mask captured by enter.
 *
 * Target: __set_BASEPRI(prev_state).
 * Host:   restores the emulated mask, decrements the nesting counter.
 */
void sched_port_exit_critical(sched_irq_state_t prev_state);

#endif /* SCHED_PORT_H */
