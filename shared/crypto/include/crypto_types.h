/*
 * crypto_types.h — pure types for the M4 crypto-service stack, no functions.
 *
 * Shared by the transport (ipc_mailbox), the protocol framing (crypto_msg),
 * the server dispatch (crypto_dispatch), and the client (crypto_service).
 * Implements NO algorithms (ADR-0006): SHA-256 / ECDSA live behind the M0+
 * back end, target-only. Design: ADR-0016 (verify), ADR-0017 (service layers).
 */
#ifndef CRYPTO_TYPES_H
#define CRYPTO_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------
 * Operation codes (ADR-0017 D3, layer 3 op handlers)
 * ----------------------------------------------------------------- */
typedef enum {
    CRYPTO_OP_NONE         = 0x00,
    CRYPTO_OP_HASH         = 0x01,   /* SHA-256 over a range */
    CRYPTO_OP_VERIFY_IMAGE = 0x02,   /* hash + signature over a flash range (ADR-0017 D1) */
    CRYPTO_OP_MAC          = 0x03    /* AES-CMAC over opaque bytes -> 16 B tag (ADR-0021 D7).
                                      * SecOC-agnostic: freshness/Data-ID/truncation are the
                                      * caller's concern (shared/secoc). An unknown key_id or a
                                      * malformed request still yields an ERROR verdict, never a
                                      * tag — the dispatcher only runs a handler that is REGISTERED
                                      * (see test_crypto_dispatch: a table without the MAC row keeps
                                      * answering 0x03 with ERROR). */
} crypto_op_t;

/* -------------------------------------------------------------------
 * Verify verdict (ADR-0016 D2/D5)
 *
 * VALID / INVALID are authenticated answers from the M0+. ERROR means "the
 * service could not produce an answer" (M0+ dead, timeout, malformed reply,
 * unknown op/key). ERROR is a DISTINCT value so the caller can log which
 * happened — but it maps to the SAME fail-safe as INVALID (never jump,
 * ADR-0008 D1 step 5).
 *
 * INVALID = 0 on purpose: a zeroed / uninitialised buffer read as a verdict
 * must mean "not valid". The safe answer is the default, by construction.
 * ----------------------------------------------------------------- */
typedef enum {
    CRYPTO_VERDICT_INVALID = 0,
    CRYPTO_VERDICT_VALID   = 1,
    CRYPTO_VERDICT_ERROR   = 2
} crypto_verdict_t;

/* -------------------------------------------------------------------
 * The envelope moved across the IPC boundary (ADR-0017 D3)
 *
 * A minimal { op_code, length, payload } request/response envelope. The
 * transport (ipc_mailbox) never inspects it — it moves an opaque buffer
 * (ADR-0018). Only crypto_msg (framing) and the dispatch/client layers read
 * these fields.
 * ----------------------------------------------------------------- */
#ifndef CRYPTO_MAX_PAYLOAD
#define CRYPTO_MAX_PAYLOAD   128U   /* signature (~72 B) + key_id + range, headroom for HASH out */
#endif

/* SHA-256 digest length, protocol-level (the HASH response payload size). The
 * target back end must agree — on CYT2B7 it equals the PDL's
 * CY_CRYPTO_SHA256_DIGEST_SIZE (32). Kept here so host-side/portable code
 * needs no vendor header. */
#define CRYPTO_SHA256_DIGEST_LEN   32U

/* Full AES-CMAC tag length, protocol-level (the MAC response payload size). The
 * target back end must agree — it equals the PDL's CY_CRYPTO_AES_BLOCK_SIZE (16).
 * SecOC truncates this to 8 B on the wire (ADR-0021 D1); truncation lives in
 * shared/secoc, not here — this op returns the full tag. */
#define CRYPTO_CMAC_TAG_LEN        16U

/* Wire size of an encoded envelope with a full payload. */
#define CRYPTO_MSG_HDR_SIZE  3U     /* op_code (1) + length (2, little-endian) */
#define CRYPTO_MSG_MAX_WIRE  (CRYPTO_MSG_HDR_SIZE + CRYPTO_MAX_PAYLOAD)

typedef struct {
    uint8_t  op_code;                     /* crypto_op_t */
    uint16_t length;                      /* payload bytes in use */
    uint8_t  payload[CRYPTO_MAX_PAYLOAD];
} crypto_msg_t;

#endif /* CRYPTO_TYPES_H */
