/*
 * secoc_shared_secret.h — the SecOC AES-CMAC shared secret (ADR-0021 D6).
 *
 * TARGET-ONLY, SINGLE-SOURCE. Compiled into BOTH nodes' CM0+ images (Node A app-
 * serving M0+ and Node B M0+) so they share one key — one file, so they can
 * never drift. It lives ONLY behind the M0+ offload: the application core never
 * includes this header (REQ-SECOC-011). Never link it into a host test or the
 * app image.
 *
 * ── HONESTY (secoc-architecture §8.3) ──
 * This is a PORTFOLIO DUMMY. A shipped fleet NEVER commits a shared secret to a
 * repo: each ECU is provisioned a per-ECU key from a backend key master (a KDF),
 * and the bytes live in a hardware-protected store, not a header. The M0+
 * offload here protects the key from DISCLOSURE (a CM7 memory bug can't read it),
 * not from a fully-compromised app calling the MAC oracle — inherent to
 * symmetric SecOC. Session/per-ECU keys are a future milestone (ADR-0021 D6).
 *
 * DO NOT reuse these bytes for anything real.
 */
#ifndef SECOC_SHARED_SECRET_H
#define SECOC_SHARED_SECRET_H

#include <stdint.h>
#include "secoc_key_id.h"   /* SECOC_MAC_KEY_ID */

#define SECOC_AES128_KEY_BYTES   16u

/* Dummy AES-128 key. Recognisably fake on purpose. */
static const uint8_t secoc_shared_secret[SECOC_AES128_KEY_BYTES] = {
    0x53u, 0x65u, 0x63u, 0x4Fu, 0x43u, 0x21u, 0xDEu, 0xADu,   /* "SecOC!.." */
    0xBEu, 0xEFu, 0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u
};

#endif /* SECOC_SHARED_SECRET_H */
