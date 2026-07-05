/*
 * isotp_types.h — ISO-TP (ISO 15765-2) transport types and config.
 *
 * Pure types, no functions (ADR-0012 D1: transport knows nothing about
 * services; hands the layer above a complete reassembled buffer). Design:
 * ADR-0012, ADR-0013.
 */
#ifndef ISOTP_TYPES_H
#define ISOTP_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* Worst-case reassembled message size -- sized to the largest transferData
 * block M3 uses (ADR-0013 D1). Override via -D in the build. */
#ifndef ISOTP_MAX_PAYLOAD
#define ISOTP_MAX_PAYLOAD   4096U
#endif

/* Flow-control parameters (ADR-0013 D3). */
#ifndef ISOTP_BLOCK_SIZE
#define ISOTP_BLOCK_SIZE    8U       /* consecutive frames per FC before another FC */
#endif
#ifndef ISOTP_STMIN_MS
#define ISOTP_STMIN_MS      0U       /* minimum separation time; CANFD data phase is fast */
#endif

/* Frame timing (ADR-0013 D3), milliseconds. */
#ifndef ISOTP_N_AS_MS
#define ISOTP_N_AS_MS       1000U
#endif
#ifndef ISOTP_N_BS_MS
#define ISOTP_N_BS_MS       1000U
#endif
#ifndef ISOTP_N_CR_MS
#define ISOTP_N_CR_MS       1000U
#endif

typedef enum {
    ISOTP_IDLE = 0,
    ISOTP_RX_IN_PROGRESS,
    ISOTP_TX_IN_PROGRESS,
    ISOTP_RX_COMPLETE,
    ISOTP_ERROR_TIMEOUT,
    ISOTP_ERROR_FLOW,
    ISOTP_ERROR_OVERFLOW
} isotp_state_t;

/* --------------------------------------------------------------------
 * PCI (protocol control info) layout -- a deliberately SIMPLIFIED variant of
 * ISO 15765-2, not byte-exact conformance. This project demonstrates the
 * SF/FF/CF/FC state machine and its safety properties, not a certified stack;
 * calling that out here is the same honesty posture as ADR-0008 D3's
 * CRC32-is-not-authenticity note.
 *
 * data[0] bits [7:4] = frame type; the rest of data[0] is unused (0) except
 * for CF/FC below.
 *
 *   SF (0x0_): data[1]     = payload length (0..62)
 *              data[2..]   = payload
 *   FF (0x1_): data[1..4]  = total length, 32-bit big-endian
 *              data[5..]   = initial payload
 *   CF (0x2_): data[0] low nibble = sequence number (0..15, wraps)
 *              data[1..]   = continuation payload
 *   FC (0x3_): data[0] low nibble = flow status (0 CTS, 1 WAIT, 2 OVERFLOW)
 *              data[1]     = block size
 *              data[2]     = STmin (ms)
 * ------------------------------------------------------------------ */
#define ISOTP_PCI_TYPE_SF   0x00U
#define ISOTP_PCI_TYPE_FF   0x10U
#define ISOTP_PCI_TYPE_CF   0x20U
#define ISOTP_PCI_TYPE_FC   0x30U

#define ISOTP_FC_STATUS_CTS       0x00U
#define ISOTP_FC_STATUS_WAIT      0x01U
#define ISOTP_FC_STATUS_OVERFLOW  0x02U

#endif /* ISOTP_TYPES_H */
