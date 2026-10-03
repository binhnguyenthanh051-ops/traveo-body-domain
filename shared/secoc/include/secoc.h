/*
 * secoc.h — SecOC secured-frame layer: build (secure) and verify+strip a
 * single CAN FD frame [ PDU | freshness(4) | trunc-MAC(8) ], with the Data ID
 * bound into the MAC input.
 *
 * Decisions: ADR-0021 D1 (CMAC, 8-B truncation, constant-time compare), D2
 * (frame layout + Data-ID-in-MAC), D9 (verify failure ⇒ drop-before-decode).
 * Requirements: REQ-SECOC-001..004, 013.
 *
 * Host-testable (ADR-0001). This module reaches out in exactly ONE direction:
 * the injected secoc_mac_if_t oracle (down to the M0+ crypto service). It does
 * NOT include crypto_msg/ipc_mailbox/CAN/flash/FreeRTOS symbols — the
 * `secoc_core` rule in checks/rules.yml enforces that. Freshness is a separate
 * module (secoc_freshness.h); this one only carries the (epoch,counter) it is
 * handed and parses the (epoch,counter) it receives.
 */
#ifndef SECOC_H
#define SECOC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SECOC_FRESHNESS_LEN   4U    /* epoch(u16 LE) ∥ counter(u16 LE) */
#define SECOC_MAC_LEN         8U    /* truncated MAC on the wire (ADR-0021 D1) */
#define SECOC_TRAILER_LEN     (SECOC_FRESHNESS_LEN + SECOC_MAC_LEN)   /* 12 */
#define SECOC_FULL_TAG_LEN    16U   /* full CMAC tag from the oracle */
#define SECOC_DATAID_LEN      2U    /* Data ID prefixed into the MAC input, NOT transmitted */

/* -------------------------------------------------------------------
 * MAC oracle port (ADR-0021 D7 seam): AES-CMAC over `msg` ⇒ 16-byte `tag`.
 * Returns false if no tag could be produced (unknown key_id, M0+ error) ⇒ the
 * caller fails safe. Target wires this to crypto_service's crypto_mac(); host
 * tests inject a deterministic fake.
 * ----------------------------------------------------------------- */
typedef struct {
    bool (*mac)(uint32_t key_id, const uint8_t *msg, size_t len,
                uint8_t tag[SECOC_FULL_TAG_LEN]);
} secoc_mac_if_t;

typedef struct {
    const secoc_mac_if_t *oracle;
    uint32_t key_id;
} secoc_crypto_t;

/* -------------------------------------------------------------------
 * TX — build a secured frame (REQ-SECOC-001..004).
 *
 * MAC input   = DataID(can_id, 2 B LE) ∥ freshness(4 B) ∥ pdu   (Data ID NOT sent)
 * Transmitted = pdu ∥ freshness(4 B) ∥ MAC[0..8)
 *
 * The (epoch,counter) come from secoc_tx_next(). Returns the secured length
 * (pdu_len + 12), or 0 on failure (no room, or the oracle produced no tag).
 * ----------------------------------------------------------------- */
size_t secoc_secure(const secoc_crypto_t *cy, uint16_t can_id,
                    uint16_t epoch, uint16_t counter,
                    const uint8_t *pdu, size_t pdu_len,
                    uint8_t *out, size_t cap);

typedef enum {
    SECOC_OK         = 0,   /* MAC verified; pdu_out/epoch/counter are valid */
    SECOC_BAD_MAC    = 1,   /* trailing MAC did not match ⇒ DROP (REQ-SECOC-001) */
    SECOC_BAD_LENGTH = 2,   /* frame too short to hold PDU + trailer */
    SECOC_MAC_ERROR  = 3    /* oracle could not produce a tag (fail-safe ⇒ DROP) */
} secoc_verify_result_t;

/* -------------------------------------------------------------------
 * RX — verify + strip (REQ-SECOC-001..004, 013).
 *
 * Recomputes the MAC over DataID(can_id) ∥ freshness ∥ pdu and compares its
 * first 8 bytes CONSTANT-TIME against the trailing MAC. Only on SECOC_OK does
 * it copy the stripped authentic PDU into pdu_out and report its length and the
 * parsed (epoch,counter). A failed MAC NEVER yields a PDU — drop-before-decode.
 * Because the Data ID is bound, a frame replayed on a different can_id fails
 * here (REQ-SECOC-004). This does NOT touch freshness state; the caller runs
 * secoc_rx_check/secoc_rx_accept.
 * ----------------------------------------------------------------- */
secoc_verify_result_t secoc_verify(const secoc_crypto_t *cy, uint16_t can_id,
                                   const uint8_t *frame, size_t frame_len,
                                   uint16_t *epoch, uint16_t *counter,
                                   uint8_t *pdu_out, size_t cap, size_t *pdu_len);

/* -------------------------------------------------------------------
 * RX with a CONFIGURED PDU length (W40 bench finding; ADR-0021 addendum).
 *
 * Same verify + strip as secoc_verify(), but the authentic PDU is the first
 * `pdu_len_cfg` bytes, as configured per CAN ID (body_msg_pdu_len), instead of
 * being inferred as frame_len - 12. Needed on a real bus: CAN FD pads a 13..15 B
 * secured frame to 16 B, so inference puts the MAC one or more bytes off and
 * every genuine frame fails. frame_len must be the exact secured length or that
 * length padded to the next CAN FD length; anything else is SECOC_BAD_LENGTH.
 * Padding bytes are not part of the MAC and are ignored.
 * ----------------------------------------------------------------- */
secoc_verify_result_t secoc_verify_len(const secoc_crypto_t *cy, uint16_t can_id,
                                       const uint8_t *frame, size_t frame_len,
                                       size_t pdu_len_cfg,
                                       uint16_t *epoch, uint16_t *counter,
                                       uint8_t *pdu_out, size_t cap, size_t *pdu_len);

/* Smallest CAN FD data-field length that holds n bytes (0..8, 12, 16, 20, 24,
 * 32, 48, 64), or 0 if n > 64. Exposed for direct test. */
size_t secoc_fd_frame_len(size_t n);

/* Constant-time equality of two n-byte buffers (ADR-0021 D1): no data-dependent
 * early-out. Exposed for direct test. Returns true iff all n bytes match. */
bool secoc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#endif /* SECOC_H */
