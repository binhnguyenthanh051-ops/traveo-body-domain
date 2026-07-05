/*
 * isotp.h — ISO-TP transport (ADR-0012 layer 1, ADR-0013).
 *
 * Segments/reassembles multi-frame UDS messages over the existing CANFD
 * channel. Poll-based (ADR-0012 D2) -- fits the FBL super-loop (ADR-0004);
 * never sees a service ID and never hands the layer above a raw CAN frame.
 *
 * Depends on can_hal_if_t (ADR-0011, unchanged) for send() and the M3
 * addition recv() (can_hal.h) for RX -- no other hardware dependency.
 * Host-testable: the host fake backs can_hal_if_t.
 */
#ifndef ISOTP_H
#define ISOTP_H

#include "isotp_types.h"
#include "can_hal.h"
#include <stdbool.h>

/* Bind the CAN instance this transport uses and the CAN ID every outgoing
 * frame (SF/FF/CF/FC) is sent under -- e.g. the Node A diagnostic response
 * ID, 0x7A8 (ADR-0002 M3 extension). Call once at composition time. */
void isotp_init(const can_hal_if_t *can, uint32_t tx_id);

/* Drain pending CAN RX, advance the SF/FF/CF/FC state machine, send flow
 * control synchronously when a reception needs a CTS. Call once per FBL
 * super-loop iteration. */
void isotp_poll(uint32_t now_ms);

/* Segment and transmit a complete message (may span multiple frames).
 * Returns 0 on send accepted, non-zero on error (e.g. length >
 * ISOTP_MAX_PAYLOAD, or a transmission already in progress). */
int isotp_send(const uint8_t *buf, size_t len);

/* If a complete message has been reassembled, copy it into buf (capacity
 * buf_cap) and report its length in *out_len; returns true and clears the
 * pending state. Returns false if nothing is ready yet. */
bool isotp_take_received(uint8_t *buf, size_t buf_cap, size_t *out_len);

/* Current transport state, mainly for host tests and diagnostics. */
isotp_state_t isotp_current_state(void);

#endif /* ISOTP_H */
