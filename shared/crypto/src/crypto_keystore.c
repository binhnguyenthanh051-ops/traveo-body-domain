/*
 * crypto_keystore.c — key_id → public key selection (ADR-0019 D3).
 *
 * Linear scan of a small, static table. An unknown key_id returns NULL — never
 * a fallback to key 0 — so an attacker cannot name away the real key by
 * supplying an id this device does not hold.
 */
#include "crypto_keystore.h"

static const crypto_key_entry_t *g_table;
static size_t g_count;

void crypto_keystore_init(const crypto_key_entry_t *table, size_t count)
{
    g_table = table;
    g_count = count;
}

const crypto_key_entry_t *crypto_keystore_lookup(uint32_t key_id)
{
    if (g_table == NULL)
    {
        return NULL;
    }
    for (size_t i = 0U; i < g_count; ++i)
    {
        if (g_table[i].key_id == key_id)
        {
            return &g_table[i];
        }
    }
    return NULL;
}
