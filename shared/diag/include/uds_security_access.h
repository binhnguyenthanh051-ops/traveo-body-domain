/*
 * uds_security_access.h — 0x27 SecurityAccess (ADR-0012 D1 layer 3, ADR-0014
 * D2/D3).
 *
 * Owns the security-access state machine (locked -> seed-sent -> unlocked)
 * privately; other handlers never see this header, only
 * uds_session_is_unlocked() (uds_session.h), which uds_session.c backs by
 * calling uds_security_access_is_unlocked() below -- a vertical
 * (session-to-handler) dependency, not a sideways one (ADR-0012 D1).
 *
 * Honesty note (ADR-0014 D3): the M3 seed/key transform is a fixed,
 * non-cryptographic algorithm. It demonstrates the state-machine mechanism,
 * not a real security boundary -- same posture as ADR-0008 D3's CRC32 note.
 */
#ifndef UDS_SECURITY_ACCESS_H
#define UDS_SECURITY_ACCESS_H

#include "uds_handler.h"
#include <stdbool.h>
#include <stdint.h>

/* requestSeed (subfunction 0x01) response payload: [0x01, seed(4, BE)].
 * sendKey (subfunction 0x02) request payload: [0x02, key(4, BE)], where
 * key = seed ^ UDS_SECURITY_KEY_XOR_CONST -- the M3 "mechanism, not security"
 * transform (see the honesty note above). Not a secret; documented so a test
 * or PC tool can compute it like any other client would. */
#define UDS_SECURITY_KEY_XOR_CONST   0xA5A5A5A5U

const uds_handler_if_t *uds_security_access_handler(void);

/* Backing query for uds_session_is_unlocked() (uds_session.h). Not intended
 * to be called directly by other handlers. */
bool uds_security_access_is_unlocked(void);

/* A session drop or S3 timeout re-locks unconditionally (ADR-0014 D2); called
 * by uds_session.c, not by other handlers. */
void uds_security_access_relock(void);

#endif /* UDS_SECURITY_ACCESS_H */
