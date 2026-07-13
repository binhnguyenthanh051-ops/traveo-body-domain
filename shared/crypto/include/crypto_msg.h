/*
 * crypto_msg.h — envelope framing for the crypto service (ADR-0017 D3, layer 2).
 *
 * Pure serialization of crypto_msg_t to/from the byte buffer the transport
 * carries. Knows the *shape* of a request; knows nothing about the mailbox
 * wire below (ADR-0018) or the boot-decision above (ADR-0016). The malformed-
 * decode path is what turns a garbled M0+ reply into a clean ERROR verdict
 * rather than a misparse (ADR-0016 D5).
 */
#ifndef CRYPTO_MSG_H
#define CRYPTO_MSG_H

#include "crypto_types.h"

/* Encode m into buf (capacity cap). Returns the number of bytes written, or 0
 * if it would not fit or m->length exceeds CRYPTO_MAX_PAYLOAD. Wire layout:
 *   [0] op_code | [1..2] length (LE) | [3..] payload[length] */
size_t crypto_msg_encode(const crypto_msg_t *m, uint8_t *buf, size_t cap);

/* Decode buf[0..len) into m. Returns false on any inconsistency (short buffer,
 * length field larger than the bytes present or than CRYPTO_MAX_PAYLOAD) —
 * i.e. a malformed reply is rejected, never partially trusted. */
bool crypto_msg_decode(const uint8_t *buf, size_t len, crypto_msg_t *m);

/* -------------------------------------------------------------------
 * VERIFY_IMAGE request payload: base (u32 LE) | len (u32 LE) | key_id (u32 LE)
 * ----------------------------------------------------------------- */
void crypto_make_verify_request(uint32_t base, uint32_t len, uint32_t key_id,
                                crypto_msg_t *m);
bool crypto_parse_verify_request(const crypto_msg_t *m,
                                 uint32_t *base, uint32_t *len, uint32_t *key_id);

/* -------------------------------------------------------------------
 * Verdict response payload: verdict (u8). op_code echoes the request.
 * ----------------------------------------------------------------- */
void crypto_make_verdict(crypto_op_t echo_op, crypto_verdict_t v, crypto_msg_t *m);
bool crypto_parse_verdict(const crypto_msg_t *m, crypto_verdict_t *v);

#endif /* CRYPTO_MSG_H */
