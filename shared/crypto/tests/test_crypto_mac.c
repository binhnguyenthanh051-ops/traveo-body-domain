/*
 * test_crypto_mac.c — MAC-op framing + the AES-secret key type (ADR-0021 D6/D7
 * / REQ-SECOC-010, 012). The framing round-trips are red against the stub MAC
 * helpers in crypto_msg.c; the keystore assertions ride the existing selection
 * logic (ADR-0019 D3) extended with an AES-secret row.
 *
 * The MAC op is deliberately SecOC-agnostic — it carries opaque bytes and
 * returns a 16-byte tag; freshness/Data-ID/truncation are the caller's concern
 * (shared/secoc). Exempt from MISRA.
 */
#include "unity.h"
#include "crypto_msg.h"
#include "crypto_keystore.h"
#include <string.h>

/* ---- keystore fixture: one AES-secret row (ADR-0021 D6) ---- */
static const uint8_t g_secret[16] = {
    0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
    0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU
};
static const crypto_key_entry_t g_keys[] = {
    { .key_id = 0x11U, .type = CRYPTO_KEY_AES_SECRET,
      .secret = g_secret, .secret_len = sizeof g_secret }
};

void setUp(void)    { crypto_keystore_init(g_keys, 1U); }
void tearDown(void) {}

/* ---- MAC request framing ---- */
/* @test REQ-SECOC-012 */
void test_mac_request_round_trip(void)
{
    const uint8_t msg[5] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU };
    crypto_msg_t m;
    crypto_make_mac_request(0x11U, msg, (uint16_t)sizeof msg, &m);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CRYPTO_OP_MAC, m.op_code);

    uint32_t key_id = 0U;
    const uint8_t *got = NULL;
    uint16_t got_len = 0U;
    TEST_ASSERT_TRUE(crypto_parse_mac_request(&m, &key_id, &got, &got_len));
    TEST_ASSERT_EQUAL_UINT32(0x11U, key_id);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)sizeof msg, got_len);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, got, sizeof msg);
}

/* @test REQ-SECOC-012 */
void test_mac_request_rejects_wrong_op(void)
{
    crypto_msg_t m;
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_VALID, &m);  /* not a MAC req */

    uint32_t key_id; const uint8_t *msg; uint16_t msg_len;
    TEST_ASSERT_FALSE(crypto_parse_mac_request(&m, &key_id, &msg, &msg_len));
}

/* ---- MAC response framing ---- */
/* @test REQ-SECOC-012 */
void test_mac_response_round_trip(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    for (uint8_t i = 0U; i < CRYPTO_CMAC_TAG_LEN; ++i) { tag[i] = (uint8_t)(0xC0U + i); }

    crypto_msg_t m;
    crypto_make_mac_response(tag, &m);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CRYPTO_OP_MAC, m.op_code);

    uint8_t got[CRYPTO_CMAC_TAG_LEN];
    memset(got, 0, sizeof got);
    TEST_ASSERT_TRUE(crypto_parse_mac_response(&m, got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tag, got, CRYPTO_CMAC_TAG_LEN);
}

/* ---- AES-secret key selection (ADR-0021 D6, reuses ADR-0019 D3 semantics) ---- */
/* @test REQ-SECOC-010 */
void test_aes_secret_lookup(void)
{
    const crypto_key_entry_t *e = crypto_keystore_lookup(0x11U);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(CRYPTO_KEY_AES_SECRET, e->type);
    TEST_ASSERT_EQUAL_PTR(g_secret, e->secret);
    TEST_ASSERT_EQUAL_size_t(sizeof g_secret, e->secret_len);
}

/* @test REQ-SECOC-010 */
void test_unknown_key_id_is_failure_not_key_zero(void)
{
    TEST_ASSERT_NULL(crypto_keystore_lookup(0x99U));
    TEST_ASSERT_NULL(crypto_keystore_lookup(0U));   /* never fall back to key 0 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mac_request_round_trip);
    RUN_TEST(test_mac_request_rejects_wrong_op);
    RUN_TEST(test_mac_response_round_trip);
    RUN_TEST(test_aes_secret_lookup);
    RUN_TEST(test_unknown_key_id_is_failure_not_key_zero);
    return UNITY_END();
}
