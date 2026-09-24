/*
 * secoc_mac_fake.c — see header. Deterministic, input-sensitive, NOT a MAC.
 * Test harness — exempt from MISRA.
 */
#include "secoc_mac_fake.h"

static bool fake_mac(uint32_t key_id, const uint8_t *msg, size_t len,
                     uint8_t tag[SECOC_FULL_TAG_LEN])
{
    if ((key_id == 0u) || (tag == NULL)) {
        return false;                    /* unknown/invalid key ⇒ no tag (REQ-SECOC-010) */
    }
    if ((msg == NULL) && (len != 0u)) {
        return false;
    }

    /* FNV-1a-ish fold, seeded by the key, with position mixed in so a byte swap
     * or reorder changes the output. */
    uint32_t s = 0x811C9DC5u ^ key_id;
    for (size_t i = 0u; i < len; ++i) {
        s ^= (uint32_t)msg[i];
        s *= 0x01000193u;
        s += (uint32_t)i;
    }
    /* Spread the state across 16 bytes with an xorshift-multiply per byte. */
    for (size_t b = 0u; b < SECOC_FULL_TAG_LEN; ++b) {
        s ^= s >> 15;
        s *= 0x2545F491u;
        s ^= s >> 13;
        tag[b] = (uint8_t)(s & 0xFFu);
    }
    return true;
}

const secoc_mac_if_t g_secoc_mac_fake = { fake_mac };
