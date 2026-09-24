/*
 * secoc_app.h — Node A (gateway) SecOC application glue (M5 seam 3, ADR-0021).
 *
 * The mirror of Node B's secoc_app with the roles swapped: the gateway is the
 * SENDER of commands (0x120 door, 0x121 light) and the RECEIVER of authenticated
 * telemetry (0x200) and the boot FRESHNESS_SYNC (0x2F0). App composition, so it
 * may name the message IDs, the crypto client, and can_raw_frame_t.
 */
#ifndef SECOC_APP_H
#define SECOC_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "can_hal.h"     /* can_raw_frame_t */
#include "body_msgs.h"   /* body_msg_t, MSG_ID_* */
#include "secoc_rx.h"    /* secoc_rx_t — the composed verdict seam (ADR-0023 D11) */

/* The receiver instance: n_accept / n_drop_mac / n_drop_fresh are the
 * REQ-SECOC-001 per-reason counters, read by a debugger. Not mirrored here —
 * this is the same storage the LOG_EVT_SECOC_* events are emitted beside, which
 * is what keeps the two from diverging (REQ-LOG-009). */
extern secoc_rx_t g_secoc_rx;

/* App-side: authentic and fresh, but the PDU did not decode. A message bug, not
 * a SecOC verdict, so it carries no contract event. */
extern volatile uint32_t g_secoc_drop_decode;

/* Boot the freshness contexts + store. No crypto; safe before the scheduler. */
void secoc_app_init(void);

/* RX: verify secured telemetry (0x200). On VALID+fresh, decode into *out
 * (BODY_MSG_SENSOR_REPORT) and return true; else count a drop and return false.
 * Calls the M0+ oracle — task context. */
bool secoc_app_verify_telemetry(const can_raw_frame_t *f, body_msg_t *out);

/* RX: verify a FRESHNESS_SYNC (0x2F0) and, if authentic+fresh, adopt the floor
 * it carries into our command-sender epoch (D5). Returns true on a valid sync. */
bool secoc_app_handle_sync(const can_raw_frame_t *f);

/* TX: build a secured door command (0x120). locked => DOOR_LOCK. */
bool secoc_app_build_door_cmd(bool locked, can_raw_frame_t *out);

/* TX: build a secured light command (0x121), brightness 0..100 %. */
bool secoc_app_build_light_cmd(uint8_t brightness_pct, can_raw_frame_t *out);

#endif /* SECOC_APP_H */
