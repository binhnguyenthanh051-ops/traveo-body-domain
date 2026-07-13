/*
 * test_crypto_msg.c — Unity tests for envelope framing (ADR-0017 D3).
 *
 * The malformed-decode cases are the load-bearing ones: a garbled M0+ reply
 * must be rejected (false), which is what lets crypto_service collapse it to
 * an ERROR verdict rather than misparse it (ADR-0016 D5).
 *
 * Fails against the current stub crypto_msg.c (step 5 implements). Test
 * harness — exempt from MISRA.
 */
#include "unity.h"
#include "crypto_msg.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_encode_decode_round_trip(void)
{
    crypto_msg_t in = { .op_code = (uint8_t)CRYPTO_OP_VERIFY_IMAGE, .length = 3U };
    in.payload[0] = 0xAAU; in.payload[1] = 0xBBU; in.payload[2] = 0xCCU;

    uint8_t buf[CRYPTO_MSG_MAX_WIRE];
    size_t n = crypto_msg_encode(&in, buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(CRYPTO_MSG_HDR_SIZE + 3U, n);

    crypto_msg_t out;
    memset(&out, 0, sizeof out);
    TEST_ASSERT_TRUE(crypto_msg_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(in.op_code, out.op_code);
    TEST_ASSERT_EQUAL_UINT16(in.length, out.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.payload, out.payload, 3U);
}

void test_encode_rejects_undersized_buffer(void)
{
    crypto_msg_t in = { .op_code = (uint8_t)CRYPTO_OP_HASH, .length = 10U };
    uint8_t small[4];
    TEST_ASSERT_EQUAL_size_t(0U, crypto_msg_encode(&in, small, sizeof small));
}

void test_decode_rejects_short_buffer(void)
{
    uint8_t buf[2] = { 0x02U, 0x00U };   /* shorter than the 3-byte header */
    crypto_msg_t out;
    TEST_ASSERT_FALSE(crypto_msg_decode(buf, sizeof buf, &out));
}

void test_decode_rejects_length_beyond_bytes_present(void)
{
    /* header claims 200 payload bytes, buffer only carries 1 */
    uint8_t buf[4] = { 0x02U, 0xC8U, 0x00U, 0x00U };
    crypto_msg_t out;
    TEST_ASSERT_FALSE(crypto_msg_decode(buf, sizeof buf, &out));
}

void test_verify_request_round_trip(void)
{
    crypto_msg_t m;
    crypto_make_verify_request(0x10040000U, 0x8000U, 7U, &m);

    uint32_t base = 0U, len = 0U, key_id = 0U;
    TEST_ASSERT_TRUE(crypto_parse_verify_request(&m, &base, &len, &key_id));
    TEST_ASSERT_EQUAL_HEX32(0x10040000U, base);
    TEST_ASSERT_EQUAL_HEX32(0x8000U, len);
    TEST_ASSERT_EQUAL_UINT32(7U, key_id);
}

void test_verdict_round_trip_and_echo(void)
{
    crypto_msg_t m;
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_VALID, &m);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CRYPTO_OP_VERIFY_IMAGE, m.op_code);

    crypto_verdict_t v = CRYPTO_VERDICT_ERROR;
    TEST_ASSERT_TRUE(crypto_parse_verdict(&m, &v));
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_VALID, v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_decode_round_trip);
    RUN_TEST(test_encode_rejects_undersized_buffer);
    RUN_TEST(test_decode_rejects_short_buffer);
    RUN_TEST(test_decode_rejects_length_beyond_bytes_present);
    RUN_TEST(test_verify_request_round_trip);
    RUN_TEST(test_verdict_round_trip_and_echo);
    return UNITY_END();
}
