/*
 * crypto_keystore.h — key_id → public key selection (ADR-0019 D3).
 *
 * A table lookup with one row today, designed for more (rotation, per-supplier
 * keys, dev vs production). An UNKNOWN key_id is a lookup FAILURE (NULL) — never
 * "default to key 0" (ADR-0019 D3): an attacker must not be able to name away
 * the real key by supplying an id we don't have.
 *
 * The public key bytes are target-only (compiled into the M0+ image, ADR-0019
 * D2); host tests bind a fixture table. This module holds NO private key and
 * NO algorithm (ADR-0006) — only the selection logic.
 */
#ifndef CRYPTO_KEYSTORE_H
#define CRYPTO_KEYSTORE_H

#include <stdint.h>
#include <stddef.h>

/* A key row holds either ECDSA public material (M4 verify) or an AES-CMAC secret
 * (M5 SecOC, ADR-0021 D6). `type` discriminates; it defaults to 0 =
 * CRYPTO_KEY_ECDSA_PUBLIC so existing verify rows keep their meaning. The secret
 * bytes are target-only (compiled into the M0+ image, ADR-0019 D2); host tests
 * bind a fixture. This module holds NO algorithm — only selection. */
typedef enum {
    CRYPTO_KEY_ECDSA_PUBLIC = 0,   /* pubkey/pubkey_len are the verification key (ADR-0019) */
    CRYPTO_KEY_AES_SECRET   = 1    /* secret/secret_len are the AES-CMAC key (ADR-0021 D6) */
} crypto_key_type_t;

typedef struct {
    uint32_t          key_id;
    const uint8_t    *pubkey;      /* SEC1/DER public point; opaque to this layer */
    size_t            pubkey_len;
    crypto_key_type_t type;        /* 0 = ECDSA public (back-compat default) */
    const uint8_t    *secret;      /* AES-CMAC secret bytes (type CRYPTO_KEY_AES_SECRET) */
    size_t            secret_len;
} crypto_key_entry_t;

/* Bind the key table (composition root: the M0+ image). */
void crypto_keystore_init(const crypto_key_entry_t *table, size_t count);

/* Return the entry for key_id, or NULL if this device holds no such key. */
const crypto_key_entry_t *crypto_keystore_lookup(uint32_t key_id);

#endif /* CRYPTO_KEYSTORE_H */
