/*
 * test_body_msgs.c — Unity tests for body_msgs pack/unpack.
 *
 * Migrated from the hand-rolled assert harness to Unity (ThrowTheSwitch).
 * Test harness — exempt from MISRA (see docs/coding-standard.md).
 */
#include "unity.h"
#include "body_msgs.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Light command ---- */

void test_light_cmd_roundtrip(void)
{
    light_cmd_msg_t in = { .brightness_pct = 75 };
    light_cmd_msg_t out = { 0 };
    uint8_t buf[8] = { 0 };

    TEST_ASSERT_EQUAL_size_t(1u, pack_light_cmd(&in, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT8(75u, buf[0]);
    TEST_ASSERT_EQUAL_INT(1, unpack_light_cmd(buf, 1u, &out));
    TEST_ASSERT_EQUAL_UINT8(75u, out.brightness_pct);
}

void test_light_cmd_rejects_out_of_range(void)
{
    light_cmd_msg_t in = { .brightness_pct = 200 };
    uint8_t buf[8] = { 0 };

    TEST_ASSERT_EQUAL_size_t(0u, pack_light_cmd(&in, buf, sizeof buf));
}

/* ---- Sensor report ---- */

void test_sensor_report_roundtrip(void)
{
    sensor_report_msg_t in  = { .ambient_raw = 0x2A5, .door_ajar = 1 };
    sensor_report_msg_t out = { 0 };
    uint8_t buf[8] = { 0 };

    TEST_ASSERT_EQUAL_size_t(3u, pack_sensor_report(&in, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT8(0x02u, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5u, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[2]);
    TEST_ASSERT_EQUAL_INT(1, unpack_sensor_report(buf, 3u, &out));
    TEST_ASSERT_EQUAL_UINT16(0x2A5, out.ambient_raw);
    TEST_ASSERT_EQUAL_UINT8(1u, out.door_ajar);
}

void test_sensor_report_rejects_short_buffer(void)
{
    sensor_report_msg_t in = { .ambient_raw = 100, .door_ajar = 0 };
    uint8_t buf[2] = { 0 };

    TEST_ASSERT_EQUAL_size_t(0u, pack_sensor_report(&in, buf, sizeof buf));
}

/* ---- Routing via decoded structs (body_decode, M2-8 seam) ---- */

void test_decode_door_cmd(void)
{
    uint8_t buf[1] = { (uint8_t)DOOR_LOCK };
    body_msg_t msg = { 0 };

    TEST_ASSERT_EQUAL_INT(1, body_decode(MSG_ID_DOOR_CMD, buf, 1u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_DOOR_CMD, msg.kind);
    TEST_ASSERT_EQUAL_INT(DOOR_LOCK, msg.u.door_cmd.cmd);
}

void test_decode_light_cmd(void)
{
    uint8_t buf[1] = { 50u };
    body_msg_t msg = { 0 };

    TEST_ASSERT_EQUAL_INT(1, body_decode(MSG_ID_LIGHT_CMD, buf, 1u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_LIGHT_CMD, msg.kind);
    TEST_ASSERT_EQUAL_UINT8(50u, msg.u.light_cmd.brightness_pct);
}

void test_decode_sensor_report(void)
{
    uint8_t buf[3] = { 0x02u, 0xA5u, 1u };
    body_msg_t msg = { 0 };

    TEST_ASSERT_EQUAL_INT(1, body_decode(MSG_ID_SENSOR_RPT, buf, 3u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_SENSOR_REPORT, msg.kind);
    TEST_ASSERT_EQUAL_UINT16(0x2A5u, msg.u.sensor_report.ambient_raw);
    TEST_ASSERT_EQUAL_UINT8(1u, msg.u.sensor_report.door_ajar);
}

void test_decode_unknown_id_rejected(void)
{
    uint8_t buf[1] = { 0u };
    body_msg_t msg = { .kind = BODY_MSG_DOOR_CMD };   /* must be overwritten to NONE */

    TEST_ASSERT_EQUAL_INT(0, body_decode(0x7FFu, buf, 1u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_NONE, msg.kind);
}

void test_decode_bad_payload_rejected(void)
{
    uint8_t light_bad[1] = { 200u };                  /* > 100 % */
    uint8_t door_bad[1]  = { 2u };                    /* not 0/1 */
    body_msg_t msg = { 0 };

    TEST_ASSERT_EQUAL_INT(0, body_decode(MSG_ID_LIGHT_CMD, light_bad, 1u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_NONE, msg.kind);
    TEST_ASSERT_EQUAL_INT(0, body_decode(MSG_ID_DOOR_CMD, door_bad, 1u, &msg));
    TEST_ASSERT_EQUAL_INT(BODY_MSG_NONE, msg.kind);
}

/* ---- Runner ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_light_cmd_roundtrip);
    RUN_TEST(test_light_cmd_rejects_out_of_range);
    RUN_TEST(test_sensor_report_roundtrip);
    RUN_TEST(test_sensor_report_rejects_short_buffer);
    RUN_TEST(test_decode_door_cmd);
    RUN_TEST(test_decode_light_cmd);
    RUN_TEST(test_decode_sensor_report);
    RUN_TEST(test_decode_unknown_id_rejected);
    RUN_TEST(test_decode_bad_payload_rejected);
    return UNITY_END();
}
