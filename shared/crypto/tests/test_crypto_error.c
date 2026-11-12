/*
 * test_crypto_error.c — LOG_EVT_CRYPTO_ERROR from the crypto client (F-018,
 * REQ-SECOC-010/012, ADR-0023 D11).
 *
 * The event existed in shared/log/events.csv but nothing emitted it, so a
 * crypto failure could only be seen with a debugger in a CRYPTO_BRINGUP_KAT=1
 * build. This suite pins the rule that makes it visible from the normal image:
 * every ERROR / false return from crypto_service emits EXACTLY ONE
 * LOG_EVT_CRYPTO_ERROR(op, reason), and a success or an INVALID verdict emits
 * nothing.
 *
 * It also gives crypto_mac() — the SecOC oracle on both nodes — its first host
 * test through the real framing and transport, faking only the port (the same
 * scripted "M0+" test_crypto_verify uses).
 *
 * Test harness — MISRA does not apply (docs/coding-standard.md).
 */
#include "unity.h"
#include "crypto_service.h"
#include "crypto_msg.h"
#include "tb_log.h"
#include "log_events.h"
#include "log_types.h"
#include "log_port_fake.h"

#include <string.h>

extern void fake_ipc_reset(void);
extern void fake_ipc_script_response(const uint8_t *resp, size_t len);
extern void fake_ipc_script_no_response(void);
extern void fake_ipc_script_deny_sema(void);
extern const ipc_port_if_t *fake_ipc_port(void);

#define KEY_ID   1U

static const uint8_t g_msg[6] = { 0x12U, 0x01U, 0x00U, 0x05U, 0xAAU, 0x55U };

/* ---- record readback (same shape as test_secoc_rx.c) --------------- */

typedef struct { uint16_t evt; uint32_t arg0; uint16_t arg1; } rec_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t take_records(rec_t *out, size_t max)
{
    (void)log_drain(0U);
    const uint8_t *b = log_fake_sink();
    size_t n = log_fake_sink_len() / LOG_REC_SIZE;
    if (n > max) { n = max; }
    for (size_t i = 0U; i < n; ++i)
    {
        const uint8_t *r = &b[i * LOG_REC_SIZE];
        out[i].evt  = rd16(&r[LOG_OFF_EVT]);
        out[i].arg0 = rd32(&r[LOG_OFF_ARG0]);
        out[i].arg1 = rd16(&r[LOG_OFF_ARG1]);
    }
    log_fake_reset_sink_only();
    return n;
}

static void assert_no_event(void)
{
    rec_t r[4];
    TEST_ASSERT_EQUAL_MESSAGE(0, take_records(r, 4U), "a success must not log a crypto error");
}

static void assert_one_error(crypto_op_t op, crypto_fail_t reason)
{
    rec_t r[4];
    size_t n = take_records(r, 4U);
    TEST_ASSERT_EQUAL_MESSAGE(1, n, "expected exactly one event for this failure");
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_CRYPTO_ERROR, r[0].evt);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)op, r[0].arg0);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)reason, r[0].arg1);
}

/* ---- scripted M0+ replies ------------------------------------------ */

static void script_msg(const crypto_msg_t *m)
{
    uint8_t buf[CRYPTO_MSG_MAX_WIRE];
    size_t n = crypto_msg_encode(m, buf, sizeof buf);
    fake_ipc_script_response(buf, n);
}

static void script_tag(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    memset(tag, 0xA5, sizeof tag);
    crypto_msg_t m;
    crypto_make_mac_response(tag, &m);
    script_msg(&m);
}

static void script_verdict(crypto_op_t echo_op, crypto_verdict_t v)
{
    crypto_msg_t m;
    crypto_make_verdict(echo_op, v, &m);
    script_msg(&m);
}

void setUp(void)
{
    log_fake_reset();
    log_init(LOG_CORE_APP);
    (void)log_drain(0U);
    log_fake_reset_sink_only();          /* discard the boot banner */

    fake_ipc_reset();
    crypto_service_init(fake_ipc_port());
}

void tearDown(void)
{
    TEST_ASSERT_TRUE_MESSAGE(log_fake_lock_balanced(), "log_port_lock/unlock unbalanced");
}

/* ---- crypto_mac: the SecOC oracle ---------------------------------- */

/* @test REQ-SECOC-012 : a tag reply yields the 16-byte tag and no error event */
void test_mac_tag_reply_returns_tag_and_logs_nothing(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN] = { 0 };
    script_tag();
    TEST_ASSERT_TRUE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xA5U, tag, CRYPTO_CMAC_TAG_LEN);
    assert_no_event();
}

/* @test REQ-SECOC-010 : an unknown key_id is refused by the M0+ and shows as CRYPTO_ERROR(MAC, REFUSED) */
void test_mac_error_verdict_logs_refused(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    script_verdict(CRYPTO_OP_MAC, CRYPTO_VERDICT_ERROR);
    TEST_ASSERT_FALSE(crypto_mac(0x99U, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_REFUSED);
}

/* @test REQ-SECOC-012 : a dead M0+ shows as CRYPTO_ERROR(MAC, IPC_TIMEOUT) */
void test_mac_timeout_logs_timeout(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    fake_ipc_script_no_response();
    TEST_ASSERT_FALSE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_IPC_TIMEOUT);
}

/* @test REQ-SECOC-012 : a contended mailbox shows as CRYPTO_ERROR(MAC, IPC_BUSY) */
void test_mac_busy_logs_busy(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    fake_ipc_script_deny_sema();
    TEST_ASSERT_FALSE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_IPC_BUSY);
}

/* @test REQ-SECOC-012 : a reply that is neither a tag nor a verdict shows as BAD_REPLY */
void test_mac_garbled_reply_logs_bad_reply(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    uint8_t garbage[4] = { 0x03U, 0xFFU, 0x00U, 0x00U };   /* length field lies */
    fake_ipc_script_response(garbage, sizeof garbage);
    TEST_ASSERT_FALSE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_BAD_REPLY);
}

/* @test REQ-SECOC-012 : a tag reply with a wrong op code is a garbled reply,
 * whatever its first byte is (F-020). A tag that starts with 0x00..0x02 used to
 * read as a verdict, so about 1 flipped-op reply in 85 was logged as REFUSED. */
void test_mac_wrong_op_reply_is_bad_reply_even_if_it_reads_as_a_verdict(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    uint8_t reply_tag[CRYPTO_CMAC_TAG_LEN];
    uint8_t buf[CRYPTO_MSG_MAX_WIRE];
    crypto_msg_t m;
    size_t n;

    memset(reply_tag, 0xA5, sizeof reply_tag);
    reply_tag[0] = (uint8_t)CRYPTO_VERDICT_ERROR;   /* looks like a verdict byte */
    crypto_make_mac_response(reply_tag, &m);
    n = crypto_msg_encode(&m, buf, sizeof buf);
    buf[0] ^= 0xFFU;                                /* the op code no longer echoes MAC */
    fake_ipc_script_response(buf, n);

    TEST_ASSERT_FALSE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_BAD_REPLY);
}

/* @test REQ-SECOC-012 : no port bound ⇒ fail-safe false, logged as REQUEST */
void test_mac_without_port_logs_request(void)
{
    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    crypto_service_init(NULL);
    TEST_ASSERT_FALSE(crypto_mac(KEY_ID, g_msg, sizeof g_msg, tag));
    assert_one_error(CRYPTO_OP_MAC, CRYPTO_FAIL_REQUEST);
}

/* ---- crypto_verify_image: the FBL's boot check ---------------------- */

/* A bad signature is an answer. Logging it as a crypto error would make a
 * tampered image look like a dead M0+. */
void test_verify_invalid_is_an_answer_not_an_error(void)
{
    script_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_INVALID);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_INVALID, crypto_verify_image(0x10040000U, 0x8000U, KEY_ID));
    assert_no_event();
}

void test_verify_valid_logs_nothing(void)
{
    script_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_VALID);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_VALID, crypto_verify_image(0x10040000U, 0x8000U, KEY_ID));
    assert_no_event();
}

void test_verify_timeout_logs_timeout(void)
{
    fake_ipc_script_no_response();
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR, crypto_verify_image(0x10040000U, 0x8000U, KEY_ID));
    assert_one_error(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_IPC_TIMEOUT);
}

/* F-020: a verdict that answers another op is a garbled reply, not an answer. */
void test_verify_wrong_op_verdict_logs_bad_reply(void)
{
    script_verdict(CRYPTO_OP_MAC, CRYPTO_VERDICT_VALID);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR, crypto_verify_image(0x10040000U, 0x8000U, KEY_ID));
    assert_one_error(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_BAD_REPLY);
}

void test_verify_error_verdict_logs_refused(void)
{
    script_verdict(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_VERDICT_ERROR);
    TEST_ASSERT_EQUAL(CRYPTO_VERDICT_ERROR, crypto_verify_image(0x10040000U, 0x8000U, KEY_ID));
    assert_one_error(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_REFUSED);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mac_tag_reply_returns_tag_and_logs_nothing);
    RUN_TEST(test_mac_error_verdict_logs_refused);
    RUN_TEST(test_mac_timeout_logs_timeout);
    RUN_TEST(test_mac_busy_logs_busy);
    RUN_TEST(test_mac_garbled_reply_logs_bad_reply);
    RUN_TEST(test_mac_wrong_op_reply_is_bad_reply_even_if_it_reads_as_a_verdict);
    RUN_TEST(test_mac_without_port_logs_request);
    RUN_TEST(test_verify_invalid_is_an_answer_not_an_error);
    RUN_TEST(test_verify_valid_logs_nothing);
    RUN_TEST(test_verify_timeout_logs_timeout);
    RUN_TEST(test_verify_wrong_op_verdict_logs_bad_reply);
    RUN_TEST(test_verify_error_verdict_logs_refused);
    return UNITY_END();
}
