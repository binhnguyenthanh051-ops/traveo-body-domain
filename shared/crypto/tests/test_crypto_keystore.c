/*
 * test_crypto_keystore.c — Unity tests for key selection (ADR-0019 D3).
 *
 * The load-bearing property: an unknown key_id returns NULL — never a
 * fallback to key 0 — so an attacker cannot name away the real key. A known
 * key_id returns its entry, and the table is designed to hold more than one
 * row even though production ships with one.
 *
 * Fails against the current stub crypto_keystore.c (always NULL): the "known
 * ⇒ found" cases fail until step 5; the "unknown ⇒ NULL" case already passes.
 * Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "crypto_keystore.h"

/* Two fixture keys (bytes are placeholders — selection logic only). */
static const uint8_t k_dev[]  = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
static const uint8_t k_prod[] = { 0xC0U, 0xFFU, 0xEEU, 0x00U };

static const crypto_key_entry_t g_keys[] = {
    { 1U, k_dev,  sizeof k_dev  },
    { 2U, k_prod, sizeof k_prod }
};

void setUp(void)  { crypto_keystore_init(g_keys, 2U); }
void tearDown(void) {}

void test_known_key_id_returns_entry(void)
{
    const crypto_key_entry_t *e = crypto_keystore_lookup(2U);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(2U, e->key_id);
    TEST_ASSERT_EQUAL_PTR(k_prod, e->pubkey);
}

void test_first_key_id_returns_entry(void)
{
    const crypto_key_entry_t *e = crypto_keystore_lookup(1U);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1U, e->key_id);
}

void test_unknown_key_id_returns_null_not_key_zero(void)
{
    TEST_ASSERT_NULL(crypto_keystore_lookup(99U));
    TEST_ASSERT_NULL(crypto_keystore_lookup(0U));   /* 0 is not a valid id here */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_key_id_returns_entry);
    RUN_TEST(test_first_key_id_returns_entry);
    RUN_TEST(test_unknown_key_id_returns_null_not_key_zero);
    return UNITY_END();
}
