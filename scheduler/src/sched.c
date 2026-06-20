/*
 * sched.c — scheduler core: task management, ready-set, tick/delay, selection.
 *
 * All logic in this file is hardware-independent and host-testable (ADR-0001).
 * Hardware interaction goes through the port interface (sched_port.h).
 * Design rationale: ADR-0009.
 *
 * Decide/perform split (ADR-0009 §D5):
 *   - Voluntary reschedule points (start/yield/delay) select the successor and
 *     update `current` synchronously — the running task is relinquishing the CPU.
 *   - sched_tick() only *requests* a switch (pends PendSV via the port); it marks
 *     unblocked tasks READY and advances round-robin, but does NOT itself promote
 *     a task to RUNNING. On target the pended PendSV performs the actual switch;
 *     on host that is observed via the fake port's switch counter.
 */
#include "sched.h"
#include "sched_port.h"

/* --------------------------------------------------------------------
 * Idle task configuration (F2)
 *
 * The idle task occupies a RESERVED slot beyond the user task table, so it
 * never consumes one of SCHED_MAX_TASKS and never appears in the user-facing
 * task count. It is the selector's fallback when no user task is ready, which
 * keeps task selection total and avoids __builtin_ctz(0) (undefined behaviour).
 * ------------------------------------------------------------------ */

#define SCHED_IDLE_ID        ((uint8_t)SCHED_MAX_TASKS)
#define SCHED_IDLE_PRIORITY  ((uint8_t)(SCHED_MAX_PRIORITIES - 1U))

#ifndef SCHED_IDLE_STACK_WORDS
#define SCHED_IDLE_STACK_WORDS 64U
#endif

/* --------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------ */

/* +1 for the reserved idle slot at index SCHED_IDLE_ID. */
static sched_tcb_t  g_tasks[SCHED_MAX_TASKS + 1U];
static uint32_t     g_idle_stack[SCHED_IDLE_STACK_WORDS];

static uint8_t      g_task_count   = 0U;          /* user tasks only */
static uint32_t     g_tick_count   = 0U;
static uint32_t     g_ready_bitmap = 0U;          /* user tasks only; idle is fallback */
static uint8_t      g_current_task = UINT8_MAX;

/* Per-priority round-robin index: which task within a priority level ran
 * least recently, so the tick can rotate to the next one. */
static uint8_t g_rr_index[SCHED_MAX_PRIORITIES];

/* --------------------------------------------------------------------
 * Idle task body
 * ------------------------------------------------------------------ */

static void sched_idle_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        /* On target: __WFI() — sleep until the next interrupt. The idle task
         * is the natural home for low-power wait. On host it is never actually
         * executed (no real context switch). */
    }
}

/* --------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

static uint8_t sched_find_highest_ready_priority(void)
{
    if (g_ready_bitmap == 0U)
    {
        return UINT8_MAX;
    }
    /*
     * MISRA C:2012 Dir 1.1 / Rule 1.2 deviation: __builtin_ctz is a compiler
     * intrinsic (no portable standard equivalent). It is guarded above so the
     * argument is never 0 (ctz(0) is undefined). See docs/coding-standard.md.
     */
    return (uint8_t)__builtin_ctz(g_ready_bitmap);
}

static uint8_t sched_count_ready_at(uint8_t priority)
{
    uint8_t count = 0U;
    for (uint8_t i = 0U; i < g_task_count; ++i)
    {
        if ((g_tasks[i].priority == priority) &&
            ((g_tasks[i].state == SCHED_STATE_READY) ||
             (g_tasks[i].state == SCHED_STATE_RUNNING)))
        {
            ++count;
        }
    }
    return count;
}

static uint8_t sched_pick_task_at_priority(uint8_t priority)
{
    uint8_t candidates[SCHED_MAX_TASKS];
    uint8_t count = 0U;

    for (uint8_t i = 0U; i < g_task_count; ++i)
    {
        if ((g_tasks[i].priority == priority) &&
            ((g_tasks[i].state == SCHED_STATE_READY) ||
             (g_tasks[i].state == SCHED_STATE_RUNNING)))
        {
            candidates[count] = i;
            ++count;
        }
    }

    if (count == 0U)
    {
        return UINT8_MAX;
    }

    uint8_t idx = g_rr_index[priority] % count;
    return candidates[idx];
}

/* Pure selection — returns the task id the scheduler would run next.
 * Falls back to the idle task when no user task is ready (never returns
 * UINT8_MAX, so callers never face an undefined selection). */
static uint8_t sched_choose_next(void)
{
    uint8_t prio = sched_find_highest_ready_priority();
    if (prio == UINT8_MAX)
    {
        return SCHED_IDLE_ID;
    }

    uint8_t next = sched_pick_task_at_priority(prio);
    if (next == UINT8_MAX)
    {
        return SCHED_IDLE_ID;
    }
    return next;
}

/* Apply a selection: demote the outgoing RUNNING task to READY, promote the
 * incoming task to RUNNING, and record it as current. A task that became
 * BLOCKED before this call keeps that state (the RUNNING guard skips it). */
static void sched_set_running(uint8_t next)
{
    if (next == UINT8_MAX)
    {
        return;
    }

    if (g_current_task != UINT8_MAX)
    {
        if (g_tasks[g_current_task].state == SCHED_STATE_RUNNING)
        {
            g_tasks[g_current_task].state = SCHED_STATE_READY;
        }
    }

    g_tasks[next].state = SCHED_STATE_RUNNING;
    g_current_task = next;
}

static void sched_update_bitmap_for(uint8_t priority)
{
    if (sched_count_ready_at(priority) > 0U)
    {
        g_ready_bitmap |= (1U << priority);
    }
    else
    {
        g_ready_bitmap &= ~(1U << priority);
    }
}

/* --------------------------------------------------------------------
 * Public API — initialisation
 * ------------------------------------------------------------------ */

void sched_init(void)
{
    for (uint8_t i = 0U; i < (uint8_t)(SCHED_MAX_TASKS + 1U); ++i)
    {
        g_tasks[i].state = SCHED_STATE_SUSPENDED;
        g_tasks[i].sp    = NULL;
    }
    for (uint8_t i = 0U; i < (uint8_t)SCHED_MAX_PRIORITIES; ++i)
    {
        g_rr_index[i] = 0U;
    }
    g_task_count   = 0U;
    g_tick_count   = 0U;
    g_ready_bitmap = 0U;
    g_current_task = UINT8_MAX;

    /* Create the built-in idle task in its reserved slot (F2). It is always
     * READY and is the selector's fallback; it is NOT in g_ready_bitmap. */
    sched_tcb_t *idle = &g_tasks[SCHED_IDLE_ID];
    idle->id          = SCHED_IDLE_ID;
    idle->entry       = sched_idle_task;
    idle->arg         = NULL;
    idle->priority    = SCHED_IDLE_PRIORITY;
    idle->state       = SCHED_STATE_READY;
    idle->delay_ticks = 0U;
    idle->stack_base  = g_idle_stack;
    idle->stack_size  = SCHED_IDLE_STACK_WORDS;
    idle->sp          = sched_port_init_stack(&g_idle_stack[SCHED_IDLE_STACK_WORDS],
                                              sched_idle_task, NULL);
}

/* --------------------------------------------------------------------
 * Public API — task management
 * ------------------------------------------------------------------ */

sched_err_t sched_task_create(sched_task_fn_t  entry,
                              void            *arg,
                              uint8_t          priority,
                              uint32_t        *stack_base,
                              uint32_t         stack_size)
{
    if (entry == NULL)
    {
        return SCHED_ERR_NULL;
    }
    if (stack_base == NULL)
    {
        return SCHED_ERR_STACK;
    }
    if (priority >= (uint8_t)SCHED_MAX_PRIORITIES)
    {
        return SCHED_ERR_PRIORITY;
    }
    if (g_task_count >= (uint8_t)SCHED_MAX_TASKS)
    {
        return SCHED_ERR_FULL;
    }

    uint8_t id = g_task_count;
    sched_tcb_t *tcb = &g_tasks[id];

    tcb->id          = id;
    tcb->entry       = entry;
    tcb->arg         = arg;
    tcb->priority    = priority;
    tcb->stack_base  = stack_base;
    tcb->stack_size  = stack_size;
    tcb->delay_ticks = 0U;
    tcb->state       = SCHED_STATE_READY;

    uint32_t *stack_top = &stack_base[stack_size];
    tcb->sp = sched_port_init_stack(stack_top, entry, arg);

    g_ready_bitmap |= (1U << priority);
    ++g_task_count;

    return SCHED_OK;
}

/* --------------------------------------------------------------------
 * Public API — scheduler control
 * ------------------------------------------------------------------ */

void sched_start(void)
{
    sched_irq_state_t ctx = sched_port_enter_critical();

    sched_set_running(sched_choose_next());

    sched_port_exit_critical(ctx);

    sched_port_start_first_task();
}

void sched_yield(void)
{
    sched_irq_state_t ctx = sched_port_enter_critical();

    if (g_current_task != UINT8_MAX)
    {
        uint8_t prio = g_tasks[g_current_task].priority;
        g_rr_index[prio] = (g_rr_index[prio] + 1U) % (uint8_t)SCHED_MAX_TASKS;
    }

    sched_set_running(sched_choose_next());

    sched_port_exit_critical(ctx);
    sched_port_trigger_switch();
}

void sched_delay(uint32_t ticks)
{
    if (ticks == 0U)
    {
        sched_yield();
        return;
    }

    sched_irq_state_t ctx = sched_port_enter_critical();

    if ((g_current_task != UINT8_MAX) && (g_current_task != SCHED_IDLE_ID))
    {
        uint8_t prio = g_tasks[g_current_task].priority;

        g_tasks[g_current_task].state       = SCHED_STATE_BLOCKED;
        g_tasks[g_current_task].delay_ticks = ticks;

        sched_update_bitmap_for(prio);

        sched_set_running(sched_choose_next());
    }

    sched_port_exit_critical(ctx);
    sched_port_trigger_switch();
}

/* --------------------------------------------------------------------
 * Tick handler (called from SysTick on target, directly in tests)
 *
 * Requests a switch but does not perform one (see file header note).
 * ------------------------------------------------------------------ */

void sched_tick(void)
{
    sched_irq_state_t ctx = sched_port_enter_critical();

    ++g_tick_count;

    uint8_t need_switch = 0U;

    /* Time-based unblocking */
    for (uint8_t i = 0U; i < g_task_count; ++i)
    {
        if ((g_tasks[i].state == SCHED_STATE_BLOCKED) &&
            (g_tasks[i].delay_ticks > 0U))
        {
            --g_tasks[i].delay_ticks;
            if (g_tasks[i].delay_ticks == 0U)
            {
                g_tasks[i].state = SCHED_STATE_READY;
                g_ready_bitmap |= (1U << g_tasks[i].priority);

                if (g_current_task != UINT8_MAX)
                {
                    if (g_tasks[i].priority < g_tasks[g_current_task].priority)
                    {
                        need_switch = 1U;
                    }
                }
            }
        }
    }

    /* Round-robin among equals at the current priority (F3): advancing the
     * index changes nothing visible unless we also request the switch. */
    if (g_current_task != UINT8_MAX)
    {
        uint8_t prio = g_tasks[g_current_task].priority;
        if (sched_count_ready_at(prio) > 1U)
        {
            g_rr_index[prio] = (g_rr_index[prio] + 1U) % (uint8_t)SCHED_MAX_TASKS;
            need_switch = 1U;
        }
    }

    sched_port_exit_critical(ctx);

    if (need_switch != 0U)
    {
        sched_port_trigger_switch();
    }
}

/* --------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------ */

uint32_t sched_get_tick_count(void)
{
    return g_tick_count;
}

uint8_t sched_get_task_count(void)
{
    return g_task_count;
}

const sched_tcb_t *sched_get_tcb(uint8_t id)
{
    if (id == SCHED_IDLE_ID)
    {
        return &g_tasks[SCHED_IDLE_ID];
    }
    if (id >= g_task_count)
    {
        return NULL;
    }
    return &g_tasks[id];
}

uint32_t sched_get_ready_bitmap(void)
{
    return g_ready_bitmap;
}

uint8_t sched_get_current_task_id(void)
{
    return g_current_task;
}

uint8_t sched_get_idle_task_id(void)
{
    return SCHED_IDLE_ID;
}
