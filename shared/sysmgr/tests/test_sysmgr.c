/*
 * test_sysmgr.c — Unity tests for the resource & lifecycle manager (ADR-0015).
 *
 * Pins down the sequential bring-up contract (one resource "current" at a
 * time, in table order), structural dependency gating (D5), the
 * retry-then-on_exhausted state machine (D6), and the DEGRADE cascade (a
 * dependency must be READY, not merely DEGRADED, to satisfy a dependent).
 *
 * Fails against the current stub sysmgr.c (step 5 implements). Test harness
 * — exempt from MISRA.
 */
#include "unity.h"
#include "sysmgr.h"

#define RES_A  0U
#define RES_B  1U

static int g_a_calls;
static int g_a_fail_count;     /* a_init fails this many times, then succeeds */
static int a_init(void) { ++g_a_calls; return (g_a_calls <= g_a_fail_count) ? -1 : 0; }

static int g_a_poll_calls;
static int g_a_ready_after;    /* a_poll_ready reports ready once calls >= this */
static int a_poll_ready(void) { ++g_a_poll_calls; return (g_a_poll_calls >= g_a_ready_after) ? 0 : -1; }

static int g_b_calls;
static int b_init(void) { ++g_b_calls; return 0; }

static int g_reset_calls;
static void fake_reset(void) { ++g_reset_calls; }

void setUp(void)
{
    g_a_calls = 0; g_a_fail_count = 0;
    g_a_poll_calls = 0; g_a_ready_after = 0;
    g_b_calls = 0;
    g_reset_calls = 0;
}
void tearDown(void) {}

/* ==================================================================
 * Independent, synchronous resource
 * ================================================================ */
void test_independent_sync_resource_ready_after_one_tick(void)
{
    sysmgr_resource_t table[1] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 0, .on_exhausted = SYSMGR_FAIL_DEGRADE }
    };
    sysmgr_init(table, 1U, fake_reset);

    sysmgr_tick(0U);

    TEST_ASSERT_EQUAL(SYSMGR_RES_READY, sysmgr_res_state(RES_A));
    TEST_ASSERT_EQUAL_INT(1, g_a_calls);
}

/* ==================================================================
 * Async resource (poll_ready) + dependency gating (D5): B's init must not
 * be called while A is still INITIALIZING.
 * ================================================================ */
void test_dependent_waits_for_async_dependency(void)
{
    g_a_ready_after = 3;
    sysmgr_resource_t table[2] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = a_poll_ready, .deinit = NULL,
          .init_timeout_ms = 10000, .max_retries = 0, .on_exhausted = SYSMGR_FAIL_DEGRADE },
        { .id = RES_B, .deps = { RES_A }, .dep_count = 1, .init = b_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 0, .on_exhausted = SYSMGR_FAIL_DEGRADE }
    };
    sysmgr_init(table, 2U, fake_reset);

    sysmgr_tick(0U);    /* calls a_init() */
    sysmgr_tick(1U);    /* poll #1: not ready */
    sysmgr_tick(2U);    /* poll #2: not ready */
    TEST_ASSERT_EQUAL(SYSMGR_RES_INITIALIZING, sysmgr_res_state(RES_A));
    TEST_ASSERT_EQUAL_INT(0, g_b_calls);   /* B must not have started yet */

    sysmgr_tick(3U);    /* poll #3: ready */
    TEST_ASSERT_EQUAL(SYSMGR_RES_READY, sysmgr_res_state(RES_A));
    TEST_ASSERT_EQUAL_INT(0, g_b_calls);   /* still not this tick */

    sysmgr_tick(4U);    /* now B starts */
    TEST_ASSERT_EQUAL_INT(1, g_b_calls);
    TEST_ASSERT_EQUAL(SYSMGR_RES_READY, sysmgr_res_state(RES_B));
    TEST_ASSERT_TRUE(sysmgr_settled());
}

/* ==================================================================
 * DEGRADE cascade: a dependency that ends up DEGRADED (not READY) must not
 * satisfy a hard dependency -- the dependent cascades to DEGRADED without
 * its init() ever being called.
 * ================================================================ */
void test_degraded_dependency_cascades_without_calling_dependent_init(void)
{
    g_a_fail_count = 1000;   /* always fails */
    sysmgr_resource_t table[2] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 0, .on_exhausted = SYSMGR_FAIL_DEGRADE },
        { .id = RES_B, .deps = { RES_A }, .dep_count = 1, .init = b_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 0, .on_exhausted = SYSMGR_FAIL_DEGRADE }
    };
    sysmgr_init(table, 2U, fake_reset);

    sysmgr_tick(0U);   /* A: attempt 1 (only attempt, max_retries=0) fails -> exhausted -> DEGRADED */
    TEST_ASSERT_EQUAL(SYSMGR_RES_DEGRADED, sysmgr_res_state(RES_A));

    sysmgr_tick(1U);   /* B: dep A is DEGRADED, not READY -> cascade, b_init never called */
    TEST_ASSERT_EQUAL(SYSMGR_RES_DEGRADED, sysmgr_res_state(RES_B));
    TEST_ASSERT_EQUAL_INT(0, g_b_calls);
    TEST_ASSERT_TRUE(sysmgr_settled());
}

/* ==================================================================
 * Retry-then-succeed (RETRY_INPLACE, within budget)
 * ================================================================ */
void test_retries_inplace_then_succeeds_within_budget(void)
{
    g_a_fail_count = 2;   /* fails twice, succeeds on the 3rd call */
    sysmgr_resource_t table[1] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 2, .on_exhausted = SYSMGR_FAIL_RETRY_INPLACE }
    };
    sysmgr_init(table, 1U, fake_reset);

    sysmgr_tick(0U);   /* attempt 1: fails */
    sysmgr_tick(1U);   /* attempt 2: fails */
    TEST_ASSERT_NOT_EQUAL(SYSMGR_RES_READY, sysmgr_res_state(RES_A));

    sysmgr_tick(2U);   /* attempt 3: succeeds */
    TEST_ASSERT_EQUAL(SYSMGR_RES_READY, sysmgr_res_state(RES_A));
    TEST_ASSERT_EQUAL_INT(3, g_a_calls);
}

/* ==================================================================
 * Retries exhausted -> SYSTEM_RESET (ADR-0015 D6: the FBL's CAN entry is
 * DEGRADE, not this -- this exercises the policy value itself, generically)
 * ================================================================ */
void test_retries_exhausted_system_reset_fires_once(void)
{
    g_a_fail_count = 1000;   /* always fails */
    sysmgr_resource_t table[1] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 1, .on_exhausted = SYSMGR_FAIL_SYSTEM_RESET }
    };
    sysmgr_init(table, 1U, fake_reset);

    sysmgr_tick(0U);   /* attempt 1: fails, retries remain */
    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    sysmgr_tick(1U);   /* attempt 2: fails, exhausted -> reset */
    TEST_ASSERT_EQUAL_INT(1, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(2, g_a_calls);

    sysmgr_tick(2U);   /* no further action once terminal */
    TEST_ASSERT_EQUAL_INT(1, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(2, g_a_calls);
}

/* ==================================================================
 * Retries exhausted -> DEGRADE (no reset)
 * ================================================================ */
void test_retries_exhausted_degrade_never_resets(void)
{
    g_a_fail_count = 1000;
    sysmgr_resource_t table[1] = {
        { .id = RES_A, .dep_count = 0, .init = a_init, .poll_ready = NULL, .deinit = NULL,
          .init_timeout_ms = 0, .max_retries = 1, .on_exhausted = SYSMGR_FAIL_DEGRADE }
    };
    sysmgr_init(table, 1U, fake_reset);

    sysmgr_tick(0U);
    sysmgr_tick(1U);

    TEST_ASSERT_EQUAL(SYSMGR_RES_DEGRADED, sysmgr_res_state(RES_A));
    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    TEST_ASSERT_TRUE(sysmgr_settled());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_independent_sync_resource_ready_after_one_tick);
    RUN_TEST(test_dependent_waits_for_async_dependency);
    RUN_TEST(test_degraded_dependency_cascades_without_calling_dependent_init);
    RUN_TEST(test_retries_inplace_then_succeeds_within_budget);
    RUN_TEST(test_retries_exhausted_system_reset_fires_once);
    RUN_TEST(test_retries_exhausted_degrade_never_resets);

    return UNITY_END();
}
