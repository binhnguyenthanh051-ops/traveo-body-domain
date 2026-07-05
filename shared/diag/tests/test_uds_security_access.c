/*
 * test_uds_security_access.c — Unity tests for 0x27 SecurityAccess (ADR-0014
 * D2/D3): the locked -> seed-sent -> unlocked state machine and the seed/key
 * transform. Drives the handler directly (not through uds_session), so these
 * tests are isolated from the session layer's own (separately tested)
 * implementation.
 *
 * Fails against the current stub uds_security_access.c (step 5 implements).
 * Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "uds_security_access.h"
#include <string.h>

void setUp(void)
{
    uds_security_access_relock();
}
void tearDown(void) {}

static uint32_t seed_from_resp(const uint8_t *resp)
{
    return ((uint32_t)resp[1] << 24) | ((uint32_t)resp[2] << 16) |
           ((uint32_t)resp[3] << 8)  |  (uint32_t)resp[4];
}

static void pack_key(uint8_t *req, uint32_t key)
{
    req[0] = 0x02U;
    req[1] = (uint8_t)(key >> 24);
    req[2] = (uint8_t)(key >> 16);
    req[3] = (uint8_t)(key >> 8);
    req[4] = (uint8_t)(key);
}

void test_locked_at_startup(void)
{
    TEST_ASSERT_FALSE(uds_security_access_is_unlocked());
}

void test_request_seed_moves_to_seed_sent_and_returns_seed(void)
{
    uint8_t req[1] = { 0x01U };
    uint8_t resp[8];
    size_t resp_len = 0U;

    uds_result_t r = uds_security_access_handler()->handle(req, sizeof req, resp, sizeof resp, &resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
    TEST_ASSERT_EQUAL_size_t(5U, resp_len);        /* [subfunction, seed(4)] */
    TEST_ASSERT_EQUAL_HEX8(0x01U, resp[0]);
    TEST_ASSERT_FALSE(uds_security_access_is_unlocked());  /* seed-sent, not yet unlocked */
}

void test_correct_key_unlocks(void)
{
    uint8_t seed_req[1] = { 0x01U };
    uint8_t seed_resp[8];
    size_t seed_resp_len = 0U;
    uds_security_access_handler()->handle(seed_req, sizeof seed_req, seed_resp, sizeof seed_resp, &seed_resp_len);
    uint32_t seed = seed_from_resp(seed_resp);

    uint8_t key_req[5];
    pack_key(key_req, seed ^ UDS_SECURITY_KEY_XOR_CONST);
    uint8_t key_resp[8];
    size_t key_resp_len = 0U;
    uds_result_t r = uds_security_access_handler()->handle(key_req, sizeof key_req, key_resp, sizeof key_resp, &key_resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
    TEST_ASSERT_TRUE(uds_security_access_is_unlocked());
}

void test_wrong_key_denies_and_relocks(void)
{
    uint8_t seed_req[1] = { 0x01U };
    uint8_t seed_resp[8];
    size_t seed_resp_len = 0U;
    uds_security_access_handler()->handle(seed_req, sizeof seed_req, seed_resp, sizeof seed_resp, &seed_resp_len);
    uint32_t seed = seed_from_resp(seed_resp);

    uint8_t key_req[5];
    pack_key(key_req, (seed ^ UDS_SECURITY_KEY_XOR_CONST) + 1U);   /* deliberately wrong */
    uint8_t key_resp[8];
    size_t key_resp_len = 0U;
    uds_result_t r = uds_security_access_handler()->handle(key_req, sizeof key_req, key_resp, sizeof key_resp, &key_resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_INVALID_KEY, r);
    TEST_ASSERT_FALSE(uds_security_access_is_unlocked());

    /* Relocked to LOCKED, not left at SEED_SENT -- sendKey again without a
     * fresh requestSeed must fail with a sequence error, not be silently
     * retried against the old seed. */
    uint8_t retry_req[5];
    pack_key(retry_req, seed ^ UDS_SECURITY_KEY_XOR_CONST);
    uint8_t retry_resp[8];
    size_t retry_resp_len = 0U;
    uds_result_t r2 = uds_security_access_handler()->handle(retry_req, sizeof retry_req, retry_resp, sizeof retry_resp, &retry_resp_len);
    TEST_ASSERT_EQUAL(UDS_NRC_REQUEST_SEQUENCE_ERROR, r2);
}

void test_send_key_without_request_seed_is_sequence_error(void)
{
    uint8_t key_req[5];
    pack_key(key_req, 0U);
    uint8_t resp[8];
    size_t resp_len = 0U;

    uds_result_t r = uds_security_access_handler()->handle(key_req, sizeof key_req, resp, sizeof resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_NRC_REQUEST_SEQUENCE_ERROR, r);
}

void test_relock_clears_unlocked_state(void)
{
    uint8_t seed_req[1] = { 0x01U };
    uint8_t seed_resp[8];
    size_t seed_resp_len = 0U;
    uds_security_access_handler()->handle(seed_req, sizeof seed_req, seed_resp, sizeof seed_resp, &seed_resp_len);
    uint32_t seed = seed_from_resp(seed_resp);

    uint8_t key_req[5];
    pack_key(key_req, seed ^ UDS_SECURITY_KEY_XOR_CONST);
    uint8_t key_resp[8];
    size_t key_resp_len = 0U;
    uds_security_access_handler()->handle(key_req, sizeof key_req, key_resp, sizeof key_resp, &key_resp_len);
    TEST_ASSERT_TRUE(uds_security_access_is_unlocked());

    uds_security_access_relock();
    TEST_ASSERT_FALSE(uds_security_access_is_unlocked());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_locked_at_startup);
    RUN_TEST(test_request_seed_moves_to_seed_sent_and_returns_seed);
    RUN_TEST(test_correct_key_unlocks);
    RUN_TEST(test_wrong_key_denies_and_relocks);
    RUN_TEST(test_send_key_without_request_seed_is_sequence_error);
    RUN_TEST(test_relock_clears_unlocked_state);

    return UNITY_END();
}
