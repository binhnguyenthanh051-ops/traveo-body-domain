/*
 * secoc_app.h — Node B SecOC application glue (M5 seam 3, ADR-0021).
 *
 * The thin shim between shared/messages and shared/can (secoc-architecture §7.1)
 * for the actuator: it owns the crypto oracle binding, the sender/receiver
 * freshness contexts, and the freshness store, and exposes three operations the
 * CAN task calls. This is APP composition (not shared/secoc), so it may name the
 * message IDs, the crypto client, and the can_raw_frame_t transport type — the
 * `secoc_core` layer rule constrains shared/secoc, not this file.
 *
 * Node B's roles (ADR-0021 §7.5): RECEIVER of commands (0x120/0x121 — actuate
 * only on VALID) and SENDER of authenticated telemetry (0x200) + the boot
 * FRESHNESS_SYNC (0x2F0, D5).
 */
#ifndef SECOC_APP_H
#define SECOC_APP_H

#include <stdbool.h>
#include "can_hal.h"     /* can_raw_frame_t */
#include "body_msgs.h"   /* body_msg_t, sensor_report_msg_t, MSG_ID_* */
#include "secoc_rx.h"    /* secoc_rx_t — the composed verdict seam (ADR-0023 D11) */

/* The receiver instance, exposed so a debugger can read the per-reason drop
 * counters REQ-SECOC-001 requires: n_accept / n_drop_mac / n_drop_fresh.
 *
 * They are NOT separate app-side counters any more. Keeping a local tally here
 * would recreate exactly the divergence REQ-LOG-009 forbids — the counter and
 * the LOG_EVT_SECOC_* event are one statement inside secoc_rx_process(), and
 * this is that same storage, not a mirror of it. */
extern secoc_rx_t g_secoc_rx;

/* Still app-side, because decoding is: an authentic, fresh frame whose PDU does
 * not decode. Not a SecOC verdict, so it has no contract event — it is a
 * message-definition bug, not an attack. */
extern volatile uint32_t g_secoc_drop_decode;

/* Boot the freshness contexts (sender + receiver) and the store. No crypto here,
 * so it is safe to call before the scheduler starts. */
void secoc_app_init(void);

/* RX: verify a received command frame (MAC + freshness) and, only if authentic
 * AND fresh, decode it into *out. Returns true only then; every failure path
 * increments a drop counter and returns false (the caller must NOT actuate).
 * Calls the M0+ MAC oracle, so run it from task context. */
bool secoc_app_verify_and_decode(const can_raw_frame_t *f, body_msg_t *out);

/* TX: build a secured telemetry frame (0x200) from a sensor report. Returns
 * false on pack/freshness/oracle failure. */
bool secoc_app_build_telemetry(const sensor_report_msg_t *rpt, can_raw_frame_t *out);

/* TX: build the boot FRESHNESS_SYNC frame (0x2F0) carrying this receiver's epoch
 * floor for the gateway, so the gateway raises its command epoch (D5). Returns
 * false on freshness/oracle failure. Send once, from task context, at startup. */
bool secoc_app_build_boot_sync(can_raw_frame_t *out);

#endif /* SECOC_APP_H */
