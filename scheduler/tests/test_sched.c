/*
 * test_sched.c — Unity tests for the scheduler core.
 *
 * Covers: task creation, task-state transitions, next-task selection via the
 * ready bitmap, tick-driven delay/unblock, and equal-priority round-robin.
 *
 * Test harness — exempt from MISRA (see docs/coding-standard.md).
 */
#include "unity.h"
#include "sched.h"
#include "sched_port.h"

/* Declarations for fake port helpers (defined in sched_port_fake.c). */
extern void     fake_port_reset(void);
extern uint32_t fake_port_get_critical_nesting(void);
extern uint32_t fake_port_get_switch_count(void);
extern uint32_t fake_port_get_started(void);
extern uint32_t fake_port_get_mask(void);

/* ---- Dummy task functions ---- */

static void task_dummy(void *arg) { (void)arg; for (;;) {} }

/* ---- Per-test stacks (statically allocated) ---- */

#define TEST_STACK_WORDS 64U
static uint32_t stack_a[TEST_STACK_WORDS];
static uint32_t stack_b[TEST_STACK_WORDS];
/* stack_c/stack_d reserved for future equal-priority and preemption tests */

/* ---- Fixtures ---- */

void setUp(void)
{
    fake_port_reset();
    sched_init();
}

void tearDown(void) {}

/* ==================================================================
 * Task creation
 * ================================================================ */

void test_create_single_task(void)
{
    sched_err_t err = sched_task_create(task_dummy, NULL, 1U, stack_a,
                                        TEST_STACK_WORDS);
    TEST_ASSERT_EQUAL(SCHED_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, sched_get_task_count());
}

void test_create_sets_ready_state(void)
{
    sched_task_create(task_dummy, NULL, 2U, stack_a, TEST_STACK_WORDS);
    const sched_tcb_t *tcb = sched_get_tcb(0U);
    TEST_ASSERT_NOT_NULL(tcb);
    TEST_ASSERT_EQUAL(SCHED_STATE_READY, tcb->state);
}

void test_create_sets_priority(void)
{
    sched_task_create(task_dummy, NULL, 3U, stack_a, TEST_STACK_WORDS);
    const sched_tcb_t *tcb = sched_get_tcb(0U);
    TEST_ASSERT_NOT_NULL(tcb);
    TEST_ASSERT_EQUAL_UINT8(3U, tcb->priority);
}

void test_create_rejects_null_entry(void)
{
    sched_err_t err = sched_task_create(NULL, NULL, 0U, stack_a,
                                        TEST_STACK_WORDS);
    TEST_ASSERT_EQUAL(SCHED_ERR_NULL, err);
    TEST_ASSERT_EQUAL_UINT8(0U, sched_get_task_count());
}

void test_create_rejects_null_stack(void)
{
    sched_err_t err = sched_task_create(task_dummy, NULL, 0U, NULL,
                                        TEST_STACK_WORDS);
    TEST_ASSERT_EQUAL(SCHED_ERR_STACK, err);
}

void test_create_rejects_priority_out_of_range(void)
{
    sched_err_t err = sched_task_create(task_dummy, NULL,
                                        (uint8_t)SCHED_MAX_PRIORITIES,
                                        stack_a, TEST_STACK_WORDS);
    TEST_ASSERT_EQUAL(SCHED_ERR_PRIORITY, err);
}

void test_create_rejects_when_full(void)
{
    /* Fill the task table. */
    static uint32_t stacks[SCHED_MAX_TASKS][TEST_STACK_WORDS];
    for (uint8_t i = 0U; i < (uint8_t)SCHED_MAX_TASKS; ++i)
    {
        sched_err_t err = sched_task_create(task_dummy, NULL, 0U,
                                            stacks[i], TEST_STACK_WORDS);
        TEST_ASSERT_EQUAL(SCHED_OK, err);
    }
    /* One more should fail. */
    sched_err_t err = sched_task_create(task_dummy, NULL, 0U,
                                        stack_a, TEST_STACK_WORDS);
    TEST_ASSERT_EQUAL(SCHED_ERR_FULL, err);
}

/* ==================================================================
 * Ready bitmap
 * ================================================================ */

void test_ready_bitmap_set_on_create(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    TEST_ASSERT_BITS_HIGH(1U << 0U, sched_get_ready_bitmap());
}

void test_ready_bitmap_multiple_priorities(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 3U, stack_b, TEST_STACK_WORDS);
    uint32_t expected = (1U << 0U) | (1U << 3U);
    TEST_ASSERT_EQUAL_HEX32(expected, sched_get_ready_bitmap());
}

/* ==================================================================
 * Next-task selection (highest priority ready task)
 * ================================================================ */

void test_start_selects_highest_priority_task(void)
{
    /* priority 2 created first, priority 0 created second */
    sched_task_create(task_dummy, NULL, 2U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 0U, stack_b, TEST_STACK_WORDS);

    sched_start();

    TEST_ASSERT_EQUAL_UINT8(1U, sched_get_current_task_id());
    const sched_tcb_t *running = sched_get_tcb(1U);
    TEST_ASSERT_NOT_NULL(running);
    TEST_ASSERT_EQUAL(SCHED_STATE_RUNNING, running->state);
    TEST_ASSERT_EQUAL_UINT8(0U, running->priority);
}

/* ==================================================================
 * Tick-driven delay and unblock
 * ================================================================ */

void test_tick_increments_count(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, sched_get_tick_count());
    sched_tick();
    TEST_ASSERT_EQUAL_UINT32(1U, sched_get_tick_count());
    sched_tick();
    TEST_ASSERT_EQUAL_UINT32(2U, sched_get_tick_count());
}

void test_delay_blocks_task(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 1U, stack_b, TEST_STACK_WORDS);
    sched_start();

    /* Task 0 is running (priority 0). Simulate sched_delay(3). */
    sched_delay(3U);

    const sched_tcb_t *tcb0 = sched_get_tcb(0U);
    TEST_ASSERT_NOT_NULL(tcb0);
    TEST_ASSERT_EQUAL(SCHED_STATE_BLOCKED, tcb0->state);
    TEST_ASSERT_EQUAL_UINT32(3U, tcb0->delay_ticks);

    /* Task 1 should now be running. */
    TEST_ASSERT_EQUAL_UINT8(1U, sched_get_current_task_id());
}

void test_delay_unblocks_after_ticks(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 1U, stack_b, TEST_STACK_WORDS);
    sched_start();

    sched_delay(3U);

    /* Tick 3 times — task 0 should unblock. */
    sched_tick();
    sched_tick();

    const sched_tcb_t *tcb0 = sched_get_tcb(0U);
    TEST_ASSERT_NOT_NULL(tcb0);
    TEST_ASSERT_EQUAL(SCHED_STATE_BLOCKED, tcb0->state);

    sched_tick();

    /* After the third tick, task 0 should be ready and preempt task 1. */
    TEST_ASSERT_EQUAL(SCHED_STATE_READY, tcb0->state);
    TEST_ASSERT_BITS_HIGH(1U << 0U, sched_get_ready_bitmap());
}

void test_delay_triggers_preemption_on_unblock(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 1U, stack_b, TEST_STACK_WORDS);
    sched_start();

    uint32_t switches_before = fake_port_get_switch_count();
    sched_delay(1U);

    /* One tick should unblock task 0 (higher priority) and trigger a switch. */
    sched_tick();
    TEST_ASSERT_GREATER_THAN_UINT32(switches_before, fake_port_get_switch_count());
}

/* ==================================================================
 * Equal-priority round-robin
 * ================================================================ */

void test_round_robin_rotates_on_tick(void)
{
    /* Two tasks at the same priority. */
    sched_task_create(task_dummy, NULL, 1U, stack_a, TEST_STACK_WORDS);
    sched_task_create(task_dummy, NULL, 1U, stack_b, TEST_STACK_WORDS);
    sched_start();

    (void)sched_get_current_task_id();

    /* After one tick, the other equal-priority task should be next. */
    sched_tick();

    /* The tick should trigger a switch since there's another task at the same
     * priority waiting. */
    TEST_ASSERT_GREATER_THAN_UINT32(0U, fake_port_get_switch_count());
}

/* ==================================================================
 * Introspection
 * ================================================================ */

void test_get_tcb_returns_null_for_invalid_id(void)
{
    TEST_ASSERT_NULL(sched_get_tcb(0U));
    TEST_ASSERT_NULL(sched_get_tcb(255U));
}

void test_current_task_before_start(void)
{
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, sched_get_current_task_id());
}

/* ==================================================================
 * Critical section balance (via fake port)
 * ================================================================ */

void test_critical_sections_balanced(void)
{
    sched_irq_state_t ctx = sched_port_enter_critical();
    TEST_ASSERT_EQUAL_UINT32(1U, fake_port_get_critical_nesting());
    sched_port_exit_critical(ctx);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_port_get_critical_nesting());
}

void test_critical_sections_nest_and_restore(void)
{
    /* F8: nested enter/exit must restore the previous mask, not force 0. */
    sched_irq_state_t outer = sched_port_enter_critical();
    sched_irq_state_t inner = sched_port_enter_critical();
    TEST_ASSERT_EQUAL_UINT32(2U, fake_port_get_critical_nesting());

    sched_port_exit_critical(inner);
    /* After leaving the inner section the mask is still raised (outer holds). */
    TEST_ASSERT_NOT_EQUAL(0U, fake_port_get_mask());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_port_get_critical_nesting());

    sched_port_exit_critical(outer);
    /* Only after the outer exit does the mask return to its original value. */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_port_get_mask());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_port_get_critical_nesting());
}

/* ==================================================================
 * Idle task (F2)
 * ================================================================ */

void test_idle_task_exists_and_is_lowest_priority(void)
{
    uint8_t idle_id = sched_get_idle_task_id();
    const sched_tcb_t *idle = sched_get_tcb(idle_id);
    TEST_ASSERT_NOT_NULL(idle);
    TEST_ASSERT_EQUAL(SCHED_STATE_READY, idle->state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(SCHED_MAX_PRIORITIES - 1U), idle->priority);
}

void test_idle_not_counted_as_user_task(void)
{
    /* sched_init created idle, but the user task count must still be zero. */
    TEST_ASSERT_EQUAL_UINT8(0U, sched_get_task_count());
}

void test_idle_runs_when_all_tasks_blocked(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_start();
    TEST_ASSERT_EQUAL_UINT8(0U, sched_get_current_task_id());

    /* The only user task blocks — nothing else is ready, so idle must run. */
    sched_delay(5U);
    TEST_ASSERT_EQUAL_UINT8(sched_get_idle_task_id(),
                            sched_get_current_task_id());

    /* User ready set is empty; idle is the fallback, not a bitmap entry. */
    TEST_ASSERT_EQUAL_HEX32(0U, sched_get_ready_bitmap());
}

void test_idle_preempted_when_task_unblocks(void)
{
    sched_task_create(task_dummy, NULL, 0U, stack_a, TEST_STACK_WORDS);
    sched_start();
    sched_delay(2U);   /* task0 blocks; idle now current */
    TEST_ASSERT_EQUAL_UINT8(sched_get_idle_task_id(),
                            sched_get_current_task_id());

    uint32_t switches_before = fake_port_get_switch_count();
    sched_tick();      /* delay -> 1 */
    sched_tick();      /* delay -> 0: task0 ready, higher prio than idle */

    /* The unblock must request a switch away from idle. */
    TEST_ASSERT_GREATER_THAN_UINT32(switches_before, fake_port_get_switch_count());
    const sched_tcb_t *tcb0 = sched_get_tcb(0U);
    TEST_ASSERT_NOT_NULL(tcb0);
    TEST_ASSERT_EQUAL(SCHED_STATE_READY, tcb0->state);
}

/* ==================================================================
 * Runner
 * ================================================================ */

int main(void)
{
    UNITY_BEGIN();

    /* Task creation */
    RUN_TEST(test_create_single_task);
    RUN_TEST(test_create_sets_ready_state);
    RUN_TEST(test_create_sets_priority);
    RUN_TEST(test_create_rejects_null_entry);
    RUN_TEST(test_create_rejects_null_stack);
    RUN_TEST(test_create_rejects_priority_out_of_range);
    RUN_TEST(test_create_rejects_when_full);

    /* Ready bitmap */
    RUN_TEST(test_ready_bitmap_set_on_create);
    RUN_TEST(test_ready_bitmap_multiple_priorities);

    /* Next-task selection */
    RUN_TEST(test_start_selects_highest_priority_task);

    /* Tick and delay */
    RUN_TEST(test_tick_increments_count);
    RUN_TEST(test_delay_blocks_task);
    RUN_TEST(test_delay_unblocks_after_ticks);
    RUN_TEST(test_delay_triggers_preemption_on_unblock);

    /* Round-robin */
    RUN_TEST(test_round_robin_rotates_on_tick);

    /* Introspection */
    RUN_TEST(test_get_tcb_returns_null_for_invalid_id);
    RUN_TEST(test_current_task_before_start);

    /* Critical section */
    RUN_TEST(test_critical_sections_balanced);
    RUN_TEST(test_critical_sections_nest_and_restore);

    /* Idle task */
    RUN_TEST(test_idle_task_exists_and_is_lowest_priority);
    RUN_TEST(test_idle_not_counted_as_user_task);
    RUN_TEST(test_idle_runs_when_all_tasks_blocked);
    RUN_TEST(test_idle_preempted_when_task_unblocks);

    return UNITY_END();
}
