/*
 * body_msgs.h — message definitions and pack/unpack for the body-domain network.
 *
 * Pure logic, no hardware dependencies. Compiles and is unit-tested on the host.
 * Introduced in EP.04 (HAL/patterns) and used by both nodes from EP.05 onward.
 */
#ifndef BODY_MSGS_H
#define BODY_MSGS_H

#include <stdint.h>
#include <stddef.h>

/* CAN IDs are allocated centrally here — see ADR-0002 for the scheme. */
#define MSG_ID_DOOR_CMD     0x120u   /* Gateway -> Actuator: command door state   */
#define MSG_ID_LIGHT_CMD    0x121u   /* Gateway -> Actuator: command light level  */
#define MSG_ID_SENSOR_RPT   0x200u   /* Actuator -> Gateway: sensor report        */

typedef enum { DOOR_LOCK = 0, DOOR_UNLOCK = 1 } door_cmd_t;

/* Door command: 1 byte payload. */
typedef struct { door_cmd_t cmd; } door_cmd_msg_t;

/* Light command: target brightness 0..100 (%). */
typedef struct { uint8_t brightness_pct; } light_cmd_msg_t;

/* Sensor report: ambient light (0..1023) + door-ajar flag, packed big-endian. */
typedef struct {
    uint16_t ambient_raw;   /* 10-bit ADC value */
    uint8_t  door_ajar;     /* 0 or 1 */
} sensor_report_msg_t;

/* Returns number of bytes written, or 0 on error (e.g. buffer too small). */
size_t pack_light_cmd(const light_cmd_msg_t *in, uint8_t *buf, size_t buf_len);
size_t pack_sensor_report(const sensor_report_msg_t *in, uint8_t *buf, size_t buf_len);

/* Returns 1 on success, 0 on error (bad length, out-of-range fields). */
int unpack_light_cmd(const uint8_t *buf, size_t len, light_cmd_msg_t *out);
int unpack_sensor_report(const uint8_t *buf, size_t len, sensor_report_msg_t *out);

/* -------------------------------------------------------------------
 * Decoded-message envelope (M2-8 seam, ADR-0010 D6)
 *
 * The RTOS-independent body-control logic consumes one of these plain structs
 * over an interface — never a CAN frame and never a FreeRTOS queue handle. The
 * CAN task decodes a frame into a body_msg_t and hands it across; host tests
 * build body_msg_t values directly.
 * ----------------------------------------------------------------- */

typedef enum {
    BODY_MSG_NONE = 0,
    BODY_MSG_DOOR_CMD,
    BODY_MSG_LIGHT_CMD,
    BODY_MSG_SENSOR_REPORT
} body_msg_kind_t;

typedef struct {
    body_msg_kind_t kind;
    union {
        door_cmd_msg_t      door_cmd;
        light_cmd_msg_t     light_cmd;
        sensor_report_msg_t sensor_report;
    } u;
} body_msg_t;

/* Decode a CAN payload (selected by id) into a body_msg_t. Returns 1 on a known,
 * valid message (out->kind set accordingly); 0 on unknown id or bad payload
 * (out->kind = BODY_MSG_NONE). Pure logic — no hardware, no FreeRTOS. */
int body_decode(uint32_t id, const uint8_t *buf, size_t len, body_msg_t *out);

#endif /* BODY_MSGS_H */
