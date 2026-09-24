/*
 * secoc_mac_fake.h — host stand-in for the AES-CMAC oracle (secoc_mac_if_t).
 *
 * DETERMINISTIC but NOT cryptographic: a keyed avalanche over key_id + every
 * message byte + position, so the framing tests can assert that changing the
 * Data ID / freshness / PDU changes the tag (and thus verification fails). It
 * is NOT a MAC and must never ship — the real tag comes from the M0+
 * Cy_Crypto_Core_V2_Cmac (ADR-0021 D1). key_id == 0 returns false to model the
 * "unknown key ⇒ oracle failure" contract (REQ-SECOC-010).
 * Test harness — exempt from MISRA.
 */
#ifndef SECOC_MAC_FAKE_H
#define SECOC_MAC_FAKE_H

#include "secoc.h"

/* A non-zero fixture key_id the fake treats as valid. */
#define SECOC_FAKE_KEY_ID   0x11u

extern const secoc_mac_if_t g_secoc_mac_fake;

#endif /* SECOC_MAC_FAKE_H */
