/*
 * sched.h — public API for the custom preemptive scheduler.
 *
 * Standalone deep-dive module (ADR-0005); not linked into product images.
 * The core logic is host-testable (ADR-0001); target-specific code lives
 * behind sched_port.h.  Design rationale: ADR-0009.
 *
 * Typical usage:
 *
 *     static uint32_t stack_a[128];
 *     static uint32_t stack_b[128];
 *
 *     sched_init();
 *     sched_task_create(task_a, NULL, 1, stack_a, 128);
 *     sched_task_create(task_b, NULL, 2, stack_b, 128);
 *     sched_start();   // does not return
 */
#ifndef SCHED_H
#define SCHED_H

#include "sched_types.h"

/* -------------------------------------------------------------------
 * Initialisation
 * ----------------------------------------------------------------- */

/*
 * Reset the scheduler to its initial state.  Must be called once before
 * any other scheduler function.  Clears the task table and ready bitmap.
 */
void sched_init(void);

/* -------------------------------------------------------------------
 * Task management
 * ----------------------------------------------------------------- */

/*
 * Create a task and place it in the READY state.
 *
 * entry      — task entry point (must not return; caught by exit hook).
 * arg        — opaque argument passed to entry via r0.
 * priority   — 0 is highest; must be < SCHED_MAX_PRIORITIES.
 * stack_base — pointer to a statically allocated uint32_t array.
 * stack_size — size of that array in uint32_t words (not bytes).
 *
 * Returns SCHED_OK on success, or an error code.
 * Must be called before sched_start(), not from a running task.
 */
sched_err_t sched_task_create(sched_task_fn_t  entry,
                              void            *arg,
                              uint8_t          priority,
                              uint32_t        *stack_base,
                              uint32_t         stack_size);

/* -------------------------------------------------------------------
 * Scheduler control
 * ----------------------------------------------------------------- */

/*
 * Start the scheduler — picks the highest-priority ready task and begins
 * execution.  Does not return.
 *
 * Must be called after at least one task has been created.
 */
void sched_start(void);

/*
 * Voluntarily yield the CPU.  The scheduler re-evaluates which task should
 * run next.  If the yielding task is still the highest-priority ready task,
 * it continues immediately.
 *
 * Must be called from task context only (not from ISR).
 */
void sched_yield(void);

/*
 * Block the calling task for a specified number of ticks.
 *
 * The task moves to BLOCKED and is automatically made READY after 'ticks'
 * calls to sched_tick().  If ticks == 0, equivalent to sched_yield().
 *
 * Must be called from task context only.
 */
void sched_delay(uint32_t ticks);

/* -------------------------------------------------------------------
 * Tick (called from the port's SysTick handler, or from tests directly)
 * ----------------------------------------------------------------- */

/*
 * Advance the scheduler by one tick.
 *
 * - Increments the global tick counter.
 * - Decrements delay_ticks for each BLOCKED task; unblocks tasks whose
 *   countdown reaches zero.
 * - Rotates equal-priority tasks at the current priority level.
 * - Triggers a context switch if a higher-priority task became ready.
 *
 * On target: called from the SysTick handler.
 * In tests:  called directly to simulate time progression.
 */
void sched_tick(void);

/* -------------------------------------------------------------------
 * Introspection (read-only queries, safe to call from any context)
 * ----------------------------------------------------------------- */

/* Current global tick count. */
uint32_t sched_get_tick_count(void);

/* Number of tasks currently registered. */
uint8_t sched_get_task_count(void);

/*
 * Pointer to a TCB by task ID (index).  Returns NULL if id is out of range.
 * The caller must not modify the TCB — this is for test assertions and debug.
 */
const sched_tcb_t *sched_get_tcb(uint8_t id);

/* The ready bitmap (one bit per priority level with ≥1 ready task). */
uint32_t sched_get_ready_bitmap(void);

/* ID of the currently running task, or UINT8_MAX if the scheduler has not started. */
uint8_t sched_get_current_task_id(void);

/*
 * ID of the built-in idle task (created by sched_init at the lowest priority,
 * always READY). It lives in a reserved slot and is NOT counted by
 * sched_get_task_count(); it is the task that runs when no user task is ready.
 * See ADR-0009 §D3 (F2).
 */
uint8_t sched_get_idle_task_id(void);

#endif /* SCHED_H */
