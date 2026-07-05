/*
 * can_hal.h — CAN transport seam (ADR-0011 D5).
 *
 * Plain types + a small interface, no vendor headers. The body-domain routing
 * logic and host tests depend only on can_raw_frame_t; the ModusToolbox CANFD
 * driver lives behind this interface on target, with a host fake for tests
 * (ADR-0001). One channel, full CAN FD, for M2.
 */
#ifndef CAN_HAL_H
#define CAN_HAL_H

#include <stdint.h>
#include <stddef.h>

#define CAN_MAX_DLEN   64U      /* CAN FD maximum payload */

/* Frame flags (bitfield in can_raw_frame_t.flags). */
#define CAN_FLAG_IDE   0x01U    /* extended 29-bit identifier */
#define CAN_FLAG_RTR   0x02U    /* remote frame */
#define CAN_FLAG_FDF   0x04U    /* CAN FD format (vs classic 2.0) */
#define CAN_FLAG_BRS   0x08U    /* bit-rate switch (FD data phase) */

/* A decoded CAN frame as transport — the queue element (ADR-0010 D5). */
typedef struct {
    uint32_t id;                /* 11- or 29-bit, per CAN_FLAG_IDE */
    uint8_t  flags;             /* CAN_FLAG_* */
    uint8_t  len;               /* payload byte count, 0..CAN_MAX_DLEN */
    uint8_t  data[CAN_MAX_DLEN];
} can_raw_frame_t;

/* Channel configuration (ADR-0011 D1): full CAN FD, two bit-rate phases. */
typedef struct {
    uint32_t nominal_bitrate;   /* arbitration phase, bit/s (e.g. 500000) */
    uint32_t data_bitrate;      /* FD data phase, bit/s   (e.g. 2000000) */
    uint8_t  channel;           /* CANFD channel index */
} can_cfg_t;

/* The transport interface. init()/send() return 0 on success, non-zero on
 * error.
 *
 * recv() (added for M3, ADR-0012/0013): an optional, non-blocking pull --
 * returns 0 and fills *out if a frame is pending, non-zero if none. NULL if
 * this instance only delivers RX out-of-band (the App's ISR-fed queue,
 * consumed directly by its own task, per the original M2 design below). The
 * FBL is a super-loop with no ISR-fed queue (ADR-0004); its instance
 * implements recv() by polling the peripheral's RX FIFO directly, and
 * shared/diag's ISO-TP transport calls it. This is additive -- the App's
 * existing M2 ISR+queue path is unaffected (its instance may simply leave
 * recv() NULL, or later implement it as a non-blocking queue pop). */
typedef struct {
    int (*init)(const can_cfg_t *cfg);
    int (*send)(const can_raw_frame_t *frame);
    int (*recv)(can_raw_frame_t *out);
} can_hal_if_t;

#endif /* CAN_HAL_H */
