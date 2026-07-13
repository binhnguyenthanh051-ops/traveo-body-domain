/*
 * test_crypto_verify.c — Unity tests for the FBL-side verify client
 * (ADR-0016 orchestration, ADR-0017). This is the milestone's load-bearing
 * host test: it drives crypto_verify_image through the REAL framing
 * (crypto_msg) and REAL transport (ipc_mailbox), faking only the hardware
 * port (a scripted loopback "M0+").
 *
 * The four outcomes the boot decision depends on:
 *   - M0+ says VALID    ⇒ VALID   (the only path that may lead to a jump)
 *   - M0+ says INVALID  ⇒ INVALID (wrong/absent signature)
 *   - M0+ never answers ⇒ ERROR   (timeout — fail-safe, ADR-0016 D5)
 *   - M0+ garbles reply ⇒ ERROR   (malformed — fail-safe)
 *   - mailbox contended ⇒ ERROR   (busy — fail-safe)
 * ERROR and INVALID are distinct values but the boot layer treats both as
 * "never jump" — that mapping is what these tests pin down.
 *
 * Fails against the current stub crypto_service.c (always ERROR): the VALID
 * and INVALID cases fail until step 5; the failure cases already pass. Test
 * harness — exempt from MISRA.
 */
#include "unity.h"
#include "crypto_service.h"
#include "crypto_msg.h"
#include <string.h>

extern void fake_ipc_reset(void);
extern void fake_ipc_script_response(const uint8_t *resp, size_t len);
extern void fake_ipc_script_no_response(void);
extern void fake_ipc_script_deny_sema(void);
extern const uint8_t *fake_ipc_last_request(size_t *out_len);
extern const ipc_port_if_t *fake_ipc_port(void);

void setUp(void)
{
    fake_ipc_reset();
    crypto_service_init(fake_ipc_port());
}
void tearDown(void) {}

/* Encode a verdict envelope and arm it as the M0+'s reply. */
static void script_verdict(crypto_verdict_t v)
{
    crypto_msg_t m;
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, v, &m);
    uint8_t buf[CRYPTO_MSG_MAX_WIRE];
    size_t n = crypto_msg_encode(&m, buf, sizeof buf);
    fake_ipc_script_response(buf, n);
}

void test_m0plus_says_valid(void)
{
    script_verdict(CRYPTO_VERDICT_VALID);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_VALID,
                      crypto_verify_image(0x10040000U, 0x8000U, 1U));
}

void test_m0plus_says_invalid(void)
{
    script_verdict(CRYPTO_VERDICT_INVALID);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_INVALID,
                      crypto_verify_image(0x10040000U, 0x8000U, 1U));
}

void test_timeout_maps_to_error(void)
{
    fake_ipc_script_no_response();
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR,
                      crypto_verify_image(0x10040000U, 0x8000U, 1U));
}

void test_malformed_reply_maps_to_error(void)
{
    uint8_t garbage[4] = { 0x02U, 0xFFU, 0x00U, 0x00U };  /* length field lies */
    fake_ipc_script_response(garbage, sizeof garbage);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR,
                      crypto_verify_image(0x10040000U, 0x8000U, 1U));
}

void test_busy_mailbox_maps_to_error(void)
{
    fake_ipc_script_deny_sema();
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR,
                      crypto_verify_image(0x10040000U, 0x8000U, 1U));
}

void test_request_carries_the_verify_parameters(void)
{
    script_verdict(CRYPTO_VERDICT_VALID);
    (void)crypto_verify_image(0x10040000U, 0x8000U, 7U);

    /* The mailbox request must decode back to the parameters we asked for —
     * proof the client sent VERIFY_IMAGE with the right range/key, not a
     * blank frame the M0+ would misinterpret. */
    size_t got_len = 0U;
    const uint8_t *got = fake_ipc_last_request(&got_len);
    crypto_msg_t m;
    TEST_ASSERT_TRUE(crypto_msg_decode(got, got_len, &m));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CRYPTO_OP_VERIFY_IMAGE, m.op_code);

    uint32_t base = 0U, len = 0U, key_id = 0U;
    TEST_ASSERT_TRUE(crypto_parse_verify_request(&m, &base, &len, &key_id));
    TEST_ASSERT_EQUAL_HEX32(0x10040000U, base);
    TEST_ASSERT_EQUAL_HEX32(0x8000U, len);
    TEST_ASSERT_EQUAL_UINT32(7U, key_id);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_m0plus_says_valid);
    RUN_TEST(test_m0plus_says_invalid);
    RUN_TEST(test_timeout_maps_to_error);
    RUN_TEST(test_malformed_reply_maps_to_error);
    RUN_TEST(test_busy_mailbox_maps_to_error);
    RUN_TEST(test_request_carries_the_verify_parameters);
    return UNITY_END();
}
