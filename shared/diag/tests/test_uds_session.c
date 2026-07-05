/*
 * test_uds_session.c — Unity tests for SID dispatch and the session-type
 * state machine (ADR-0012 D1/D3, ADR-0014 D1), exercised against the fake
 * transport (isotp_fake.c) so these tests are isolated from ISO-TP's own
 * (separately tested) implementation.
 *
 * Also exercises the real (currently stubbed) 0x10 SessionControl and 0x11
 * ECUReset handlers through dispatch, per the design note in
 * shared/diag/README.md: their logic is thin enough to test via the session
 * layer that calls them, rather than in dedicated near-empty test files.
 *
 * Fails against the current stub uds_session.c / uds_session_control.c /
 * uds_ecu_reset.c (step 5 implements). Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "uds_session.h"
#include "uds_session_control.h"
#include "uds_ecu_reset.h"
#include <string.h>

extern void fake_isotp_reset(void);
extern void fake_isotp_inject_received(const uint8_t *buf, size_t len);
extern const uint8_t *fake_isotp_last_sent(size_t *out_len);
extern size_t fake_isotp_send_count(void);

/* A minimal test-local handler (SID 0x99) to test dispatch mechanics without
 * depending on any real service's business logic. */
#define TEST_SID_ECHO  0x99U
static uds_result_t echo_handle(const uint8_t *req, size_t req_len,
                                 uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    (void)resp_cap;
    memcpy(resp, req, req_len);
    *resp_len = req_len;
    return UDS_NRC_NONE;
}
static const uds_handler_if_t g_echo_if = { .sid = TEST_SID_ECHO, .handle = echo_handle };

static const uds_handler_if_t *g_table[3];

void setUp(void)
{
    fake_isotp_reset();
    g_table[0] = &g_echo_if;
    g_table[1] = uds_session_control_handler();
    g_table[2] = uds_ecu_reset_handler();
    uds_session_init(g_table, 3U);
}
void tearDown(void) {}

/* ==================================================================
 * Dispatch (ADR-0012 D3)
 * ================================================================ */
void test_dispatch_routes_to_matching_sid(void)
{
    uint8_t req[] = { TEST_SID_ECHO, 0xAA, 0xBB };
    fake_isotp_inject_received(req, sizeof req);

    uds_session_tick(0U);

    size_t len = 0U;
    const uint8_t *sent = fake_isotp_last_sent(&len);
    TEST_ASSERT_EQUAL_size_t(1U, fake_isotp_send_count());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(req, sent, sizeof req);
    TEST_ASSERT_EQUAL_size_t(sizeof req, len);
}

void test_dispatch_unknown_sid_returns_service_not_supported(void)
{
    uint8_t req[] = { 0xAB, 0x01 };
    fake_isotp_inject_received(req, sizeof req);

    uds_session_tick(0U);

    size_t len = 0U;
    const uint8_t *sent = fake_isotp_last_sent(&len);
    TEST_ASSERT_EQUAL_size_t(3U, len);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_NEGATIVE_RESPONSE, sent[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, sent[1]);
    TEST_ASSERT_EQUAL_HEX8(UDS_NRC_SERVICE_NOT_SUPPORTED, sent[2]);
}

void test_dispatch_no_request_sends_nothing(void)
{
    uds_session_tick(0U);
    TEST_ASSERT_EQUAL_size_t(0U, fake_isotp_send_count());
}

/* ==================================================================
 * 0x10 DiagnosticSessionControl -- session-type transition (ADR-0014 D1)
 * ================================================================ */
void test_session_control_default_at_startup(void)
{
    TEST_ASSERT_EQUAL(UDS_SESS_DEFAULT, uds_session_current());
}

void test_session_control_programming_request_transitions(void)
{
    uint8_t req[] = { UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x02U }; /* programmingSession */
    fake_isotp_inject_received(req, sizeof req);

    uds_session_tick(0U);

    size_t len = 0U;
    const uint8_t *sent = fake_isotp_last_sent(&len);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_DIAGNOSTIC_SESSION_CONTROL, sent[0]); /* positive response */
    TEST_ASSERT_EQUAL(UDS_SESS_PROGRAMMING, uds_session_current());
}

/* ==================================================================
 * 0x11 ECUReset -- signals the composition root, never resets itself
 * (ADR-0012 D6 / uds_ecu_reset.h)
 * ================================================================ */
void test_ecu_reset_accepted_reports_reset_requested_event(void)
{
    uint8_t req[] = { UDS_SID_ECU_RESET, 0x01U }; /* hardReset */
    fake_isotp_inject_received(req, sizeof req);

    uds_session_event_t ev = uds_session_tick(0U);

    TEST_ASSERT_EQUAL(UDS_SESSION_EVENT_RESET_REQUESTED, ev);
    size_t len = 0U;
    const uint8_t *sent = fake_isotp_last_sent(&len);
    TEST_ASSERT_EQUAL_HEX8(UDS_SID_ECU_RESET, sent[0]); /* positive response was sent first */
}

void test_ordinary_tick_reports_no_event(void)
{
    TEST_ASSERT_EQUAL(UDS_SESSION_EVENT_NONE, uds_session_tick(0U));
}

/* ==================================================================
 * Runner
 * ================================================================ */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_dispatch_routes_to_matching_sid);
    RUN_TEST(test_dispatch_unknown_sid_returns_service_not_supported);
    RUN_TEST(test_dispatch_no_request_sends_nothing);

    RUN_TEST(test_session_control_default_at_startup);
    RUN_TEST(test_session_control_programming_request_transitions);

    RUN_TEST(test_ecu_reset_accepted_reports_reset_requested_event);
    RUN_TEST(test_ordinary_tick_reports_no_event);

    return UNITY_END();
}
