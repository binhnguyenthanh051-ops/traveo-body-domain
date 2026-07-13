/*
 * test_crypto_dispatch.c — Unity tests for M0+-side dispatch (ADR-0017 D3).
 *
 * The load-bearing property: the dispatcher ALWAYS answers. A known op routes
 * to its handler; an UNKNOWN op — including the reserved-but-unbuilt MAC (0x03,
 * ADR-0017 D3) — and a malformed request both produce an ERROR verdict, never
 * a silent drop and never a crash. A fake handler stands in for the real
 * SHA/ECDSA back end (target-only, ADR-0017 layer 4).
 *
 * Fails against the current stub crypto_dispatch.c (produces no response).
 * Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "crypto_dispatch.h"
#include "crypto_msg.h"
#include <string.h>

/* Fake VERIFY_IMAGE handler: always answers VALID. */
static bool fake_verify_handler(const crypto_msg_t *req, crypto_msg_t *resp)
{
    (void)req;
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_VALID, resp);
    return true;
}

static const crypto_handler_if_t g_handlers[] = {
    { CRYPTO_OP_VERIFY_IMAGE, fake_verify_handler }
};

void setUp(void)  { crypto_dispatch_init(g_handlers, 1U); }
void tearDown(void) {}

static crypto_verdict_t dispatch_and_read_verdict(const crypto_msg_t *req_msg)
{
    uint8_t req[CRYPTO_MSG_MAX_WIRE];
    size_t req_len = crypto_msg_encode(req_msg, req, sizeof req);

    uint8_t resp[CRYPTO_MSG_MAX_WIRE]; size_t resp_len = 0U;
    bool answered = crypto_dispatch(req, req_len, resp, sizeof resp, &resp_len);
    TEST_ASSERT_TRUE(answered);

    crypto_msg_t resp_msg;
    TEST_ASSERT_TRUE(crypto_msg_decode(resp, resp_len, &resp_msg));
    crypto_verdict_t v = CRYPTO_VERDICT_INVALID;
    TEST_ASSERT_TRUE(crypto_parse_verdict(&resp_msg, &v));
    return v;
}

void test_known_op_routes_to_handler(void)
{
    crypto_msg_t req;
    crypto_make_verify_request(0x10040000U, 0x8000U, 1U, &req);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_VALID, dispatch_and_read_verdict(&req));
}

void test_unknown_op_yields_error_verdict(void)
{
    crypto_msg_t req = { .op_code = 0x03U /* reserved MAC, not built */, .length = 0U };
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR, dispatch_and_read_verdict(&req));
}

void test_malformed_request_yields_error_verdict(void)
{
    /* header claims 50 payload bytes but only 1 is present */
    uint8_t req[4] = { 0x02U, 0x32U, 0x00U, 0x00U };
    uint8_t resp[CRYPTO_MSG_MAX_WIRE]; size_t resp_len = 0U;

    TEST_ASSERT_TRUE(crypto_dispatch(req, sizeof req, resp, sizeof resp, &resp_len));

    crypto_msg_t resp_msg;
    TEST_ASSERT_TRUE(crypto_msg_decode(resp, resp_len, &resp_msg));
    crypto_verdict_t v = CRYPTO_VERDICT_INVALID;
    TEST_ASSERT_TRUE(crypto_parse_verdict(&resp_msg, &v));
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR, v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_op_routes_to_handler);
    RUN_TEST(test_unknown_op_yields_error_verdict);
    RUN_TEST(test_malformed_request_yields_error_verdict);
    return UNITY_END();
}
