/*
 * test_bodyctl.c — Unity tests for the body-control state machine (ADR-0010 D6).
 *
 * Drives bodyctl_step() with hand-built body_msg_t structs only — no FreeRTOS,
 * no CAN, no queue handle (the M2-8 seam in action). Test harness — exempt from
 * MISRA (see docs/coding-standard.md).
 */
#include "unity.h"
#include "bodyctl.h"

static bodyctl_state_t  st;
static bodyctl_output_t out;

void setUp(void)    { bodyctl_init(&st); }
void tearDown(void) {}

/* ---- init ---- */

void test_init_is_safe_default(void)
{
    TEST_ASSERT_FALSE(st.door_locked);
    TEST_ASSERT_EQUAL_UINT8(0u, st.light_pct);
    TEST_ASSERT_FALSE(st.door_ajar);
}

/* ---- door command ---- */

void test_door_lock_sets_state_and_command(void)
{
    body_msg_t in = { .kind = BODY_MSG_DOOR_CMD, .u.door_cmd = { .cmd = DOOR_LOCK } };
    bodyctl_step(&st, &in, &out);

    TEST_ASSERT_TRUE(st.door_locked);
    TEST_ASSERT_TRUE(out.lock_cmd_valid);
    TEST_ASSERT_TRUE(out.lock_locked);
    TEST_ASSERT_FALSE(out.light_cmd_valid);   /* a door command emits no light command */
}

void test_door_unlock_clears_state(void)
{
    body_msg_t lock   = { .kind = BODY_MSG_DOOR_CMD, .u.door_cmd = { .cmd = DOOR_LOCK } };
    body_msg_t unlock = { .kind = BODY_MSG_DOOR_CMD, .u.door_cmd = { .cmd = DOOR_UNLOCK } };
    bodyctl_step(&st, &lock, &out);
    bodyctl_step(&st, &unlock, &out);

    TEST_ASSERT_FALSE(st.door_locked);
    TEST_ASSERT_TRUE(out.lock_cmd_valid);
    TEST_ASSERT_FALSE(out.lock_locked);
}

/* ---- light command ---- */

void test_light_cmd_sets_level(void)
{
    body_msg_t in = { .kind = BODY_MSG_LIGHT_CMD, .u.light_cmd = { .brightness_pct = 60 } };
    bodyctl_step(&st, &in, &out);

    TEST_ASSERT_EQUAL_UINT8(60u, st.light_pct);
    TEST_ASSERT_TRUE(out.light_cmd_valid);
    TEST_ASSERT_EQUAL_UINT8(60u, out.light_pct);
}

/* ---- sensor report courtesy-light rule ---- */

void test_door_ajar_turns_light_full_on(void)
{
    body_msg_t in = { .kind = BODY_MSG_SENSOR_REPORT,
                      .u.sensor_report = { .ambient_raw = 100, .door_ajar = 1 } };
    bodyctl_step(&st, &in, &out);

    TEST_ASSERT_TRUE(st.door_ajar);
    TEST_ASSERT_TRUE(out.light_cmd_valid);
    TEST_ASSERT_EQUAL_UINT8(100u, out.light_pct);
}

void test_door_closed_turns_light_off(void)
{
    body_msg_t ajar   = { .kind = BODY_MSG_SENSOR_REPORT,
                          .u.sensor_report = { .ambient_raw = 100, .door_ajar = 1 } };
    body_msg_t closed = { .kind = BODY_MSG_SENSOR_REPORT,
                          .u.sensor_report = { .ambient_raw = 100, .door_ajar = 0 } };
    bodyctl_step(&st, &ajar, &out);
    bodyctl_step(&st, &closed, &out);

    TEST_ASSERT_FALSE(st.door_ajar);
    TEST_ASSERT_TRUE(out.light_cmd_valid);
    TEST_ASSERT_EQUAL_UINT8(0u, out.light_pct);
}

/* ---- non-events ---- */

void test_none_is_noop(void)
{
    body_msg_t in = { .kind = BODY_MSG_NONE };
    bodyctl_step(&st, &in, &out);

    TEST_ASSERT_FALSE(out.lock_cmd_valid);
    TEST_ASSERT_FALSE(out.light_cmd_valid);
}

void test_null_input_clears_output_without_crash(void)
{
    bodyctl_step(&st, NULL, &out);

    TEST_ASSERT_FALSE(out.lock_cmd_valid);
    TEST_ASSERT_FALSE(out.light_cmd_valid);
}

/* ---- runner ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_is_safe_default);
    RUN_TEST(test_door_lock_sets_state_and_command);
    RUN_TEST(test_door_unlock_clears_state);
    RUN_TEST(test_light_cmd_sets_level);
    RUN_TEST(test_door_ajar_turns_light_full_on);
    RUN_TEST(test_door_closed_turns_light_off);
    RUN_TEST(test_none_is_noop);
    RUN_TEST(test_null_input_clears_output_without_crash);
    return UNITY_END();
}
