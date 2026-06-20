/*
 * sched_port_fake.c — host-side fake port for scheduler unit tests.
 *
 * Provides simplified implementations of the five port functions so the
 * scheduler core can be tested on x86 without any ARM dependencies.
 * Test harness — exempt from MISRA (see docs/coding-standard.md).
 */
#include "sched_port.h"
#include <string.h>

/* ---- State visible to tests for assertions ---- */

/* Emulated interrupt mask — a stand-in for BASEPRI. Raised inside a critical
 * section; the previous value is returned to the caller and restored on exit. */
#define FAKE_PORT_CEILING 0x40U

static uint32_t g_critical_nesting = 0U;
static uint32_t g_switch_triggered = 0U;
static uint32_t g_scheduler_started = 0U;
static uint32_t g_emulated_mask = 0U;

uint32_t fake_port_get_critical_nesting(void) { return g_critical_nesting; }
uint32_t fake_port_get_switch_count(void)     { return g_switch_triggered; }
uint32_t fake_port_get_started(void)          { return g_scheduler_started; }
uint32_t fake_port_get_mask(void)             { return g_emulated_mask; }

void fake_port_reset(void)
{
    g_critical_nesting = 0U;
    g_switch_triggered = 0U;
    g_scheduler_started = 0U;
    g_emulated_mask = 0U;
}

/* ---- Port interface implementation ---- */

/*
 * Build a simplified initial stack frame.  On the host we don't need a real
 * Cortex-M exception frame — just enough structure that the core's sp
 * bookkeeping works and tests can inspect the frame.
 *
 * Layout (growing downward from stack_top):
 *   [top-1]  xPSR  = 0x01000000 (Thumb bit)
 *   [top-2]  PC    = entry
 *   [top-3]  LR    = 0 (exit hook — not wired on host)
 *   [top-4]  r12   = 0
 *   [top-5]  r3    = 0
 *   [top-6]  r2    = 0
 *   [top-7]  r1    = 0
 *   [top-8]  r0    = arg
 *   --- software saved ---
 *   [top-9..top-16]  r4–r11 = 0
 */
uint32_t *sched_port_init_stack(uint32_t       *stack_top,
                                sched_task_fn_t entry,
                                void           *arg)
{
    uint32_t *sp = stack_top;

    /* Hardware auto-stacked frame (8 words) */
    *(--sp) = 0x01000000U;                  /* xPSR — Thumb bit set */
    *(--sp) = (uint32_t)(uintptr_t)entry;   /* PC                   */
    *(--sp) = 0U;                           /* LR (exit hook)       */
    *(--sp) = 0U;                           /* r12                  */
    *(--sp) = 0U;                           /* r3                   */
    *(--sp) = 0U;                           /* r2                   */
    *(--sp) = 0U;                           /* r1                   */
    *(--sp) = (uint32_t)(uintptr_t)arg;     /* r0 = arg             */

    /* Software-saved registers (8 words: r4–r11) */
    *(--sp) = 0U;   /* r11 */
    *(--sp) = 0U;   /* r10 */
    *(--sp) = 0U;   /* r9  */
    *(--sp) = 0U;   /* r8  */
    *(--sp) = 0U;   /* r7  */
    *(--sp) = 0U;   /* r6  */
    *(--sp) = 0U;   /* r5  */
    *(--sp) = 0U;   /* r4  */

    return sp;
}

void sched_port_start_first_task(void)
{
    g_scheduler_started = 1U;
}

void sched_port_trigger_switch(void)
{
    ++g_switch_triggered;
}

sched_irq_state_t sched_port_enter_critical(void)
{
    sched_irq_state_t prev = (sched_irq_state_t)g_emulated_mask;
    g_emulated_mask = FAKE_PORT_CEILING;
    ++g_critical_nesting;
    return prev;
}

void sched_port_exit_critical(sched_irq_state_t prev_state)
{
    g_emulated_mask = (uint32_t)prev_state;
    if (g_critical_nesting > 0U)
    {
        --g_critical_nesting;
    }
}
