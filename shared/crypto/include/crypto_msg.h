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

/* -------------------------------------------------------------------
 * MAC op (ADR-0021 D7 / REQ-SECOC-012).
 *
 * Request payload:  key_id (u32 LE) | msg_len (u16 LE) | msg[msg_len]
 * Response payload: tag[CRYPTO_CMAC_TAG_LEN]   (op_code echoes CRYPTO_OP_MAC)
 *
 * The op MACs opaque bytes; it knows nothing of SecOC framing. On the server a
 * lookup miss / malformed request produces an ERROR verdict (crypto_make_verdict),
 * not a tag — so a bad request is never mistaken for a valid MAC.
 * ----------------------------------------------------------------- */

/* Build a MAC request. msg_len must leave room for the 6-byte header within
 * CRYPTO_MAX_PAYLOAD; on overflow m is set to an empty (length 0) request. */
void crypto_make_mac_request(uint32_t key_id, const uint8_t *msg, uint16_t msg_len,
                             crypto_msg_t *m);

/* Parse a MAC request. *msg points INTO m->payload (valid while m is). Returns
 * false on wrong op, short buffer, or a msg_len inconsistent with m->length. */
bool crypto_parse_mac_request(const crypto_msg_t *m, uint32_t *key_id,
                              const uint8_t **msg, uint16_t *msg_len);

/* Build a MAC response carrying the full 16-byte tag. */
void crypto_make_mac_response(const uint8_t tag[CRYPTO_CMAC_TAG_LEN], crypto_msg_t *m);

/* Parse a MAC response into tag[16]. Returns false on wrong op or short payload. */
bool crypto_parse_mac_response(const crypto_msg_t *m, uint8_t tag[CRYPTO_CMAC_TAG_LEN]);

#endif /* CRYPTO_MSG_H */
