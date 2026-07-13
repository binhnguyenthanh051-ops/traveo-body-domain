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

typedef struct {
    uint32_t       key_id;
    const uint8_t *pubkey;      /* SEC1/DER public point; opaque to this layer */
    size_t         pubkey_len;
} crypto_key_entry_t;

/* Bind the key table (composition root: the M0+ image). */
void crypto_keystore_init(const crypto_key_entry_t *table, size_t count);

/* Return the entry for key_id, or NULL if this device holds no such key. */
const crypto_key_entry_t *crypto_keystore_lookup(uint32_t key_id);

#endif /* CRYPTO_KEYSTORE_H */
