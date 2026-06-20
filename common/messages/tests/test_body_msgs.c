/* Host-side unit tests for body_msgs. Tiny hand-rolled assert harness — no
 * framework needed for a skeleton; migrating to Unity (ThrowTheSwitch) is an early task — see docs/workflow.md. */
#include "body_msgs.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

static void test_light_cmd_roundtrip(void)
{
    light_cmd_msg_t in = { .brightness_pct = 75 }, out = { 0 };
    uint8_t buf[8] = { 0 };
    CHECK(pack_light_cmd(&in, buf, sizeof buf) == 1u);
    CHECK(buf[0] == 75u);
    CHECK(unpack_light_cmd(buf, 1u, &out) == 1);
    CHECK(out.brightness_pct == 75u);
}

static void test_light_cmd_rejects_out_of_range(void)
{
    light_cmd_msg_t in = { .brightness_pct = 200 };
    uint8_t buf[8] = { 0 };
    CHECK(pack_light_cmd(&in, buf, sizeof buf) == 0u);   /* >100 rejected */
}

static void test_sensor_report_roundtrip(void)
{
    sensor_report_msg_t in = { .ambient_raw = 0x2A5, .door_ajar = 1 }, out = { 0 };
    uint8_t buf[8] = { 0 };
    CHECK(pack_sensor_report(&in, buf, sizeof buf) == 3u);
    CHECK(buf[0] == 0x02u && buf[1] == 0xA5u && buf[2] == 1u);  /* big-endian */
    CHECK(unpack_sensor_report(buf, 3u, &out) == 1);
    CHECK(out.ambient_raw == 0x2A5 && out.door_ajar == 1u);
}

static void test_sensor_report_rejects_short_buffer(void)
{
    sensor_report_msg_t in = { .ambient_raw = 100, .door_ajar = 0 };
    uint8_t buf[2] = { 0 };
    CHECK(pack_sensor_report(&in, buf, sizeof buf) == 0u);  /* needs 3 bytes */
}

int main(void)
{
    printf("test_body_msgs:\n");
    test_light_cmd_roundtrip();
    test_light_cmd_rejects_out_of_range();
    test_sensor_report_roundtrip();
    test_sensor_report_rejects_short_buffer();
    if (failures == 0) { printf("  all passed\n"); return 0; }
    printf("  %d check(s) failed\n", failures);
    return 1;
}
