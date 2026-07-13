/*
 * test_ipc_mailbox.c — Unity tests for the cross-core transport (ADR-0018).
 *
 * The timeout case is the load-bearing one: an M0+ that never answers must
 * return IPC_TIMEOUT within the bound and release the semaphore — that is the
 * transport half of ADR-0016 D5's "never hang, fall to fail-safe". BUSY and
 * MALFORMED are the other non-OK reasons the caller maps to ERROR.
 *
 * Fails against the current stub ipc_mailbox.c (always IPC_TIMEOUT): the OK
 * and BUSY cases fail until step 5; the timeout case already passes. Test
 * harness — exempt from MISRA.
 */
#include "unity.h"
#include "ipc_mailbox.h"
#include <string.h>

extern void fake_ipc_reset(void);
extern void fake_ipc_script_response(const uint8_t *resp, size_t len);
extern void fake_ipc_script_no_response(void);
extern void fake_ipc_script_deny_sema(void);
extern int  fake_ipc_notify_count(void);
extern bool fake_ipc_sema_held(void);
extern const uint8_t *fake_ipc_last_request(size_t *out_len);
extern const ipc_port_if_t *fake_ipc_port(void);

void setUp(void)  { fake_ipc_reset(); }
void tearDown(void) {}

void test_successful_transaction_returns_response(void)
{
    uint8_t canned[4] = { 0x02U, 0x01U, 0x00U, (uint8_t)0x01 };  /* a stand-in reply */
    fake_ipc_script_response(canned, sizeof canned);

    uint8_t req[3] = { 0x02U, 0x00U, 0x00U };
    uint8_t resp[16]; size_t resp_len = 0U;
    ipc_status_t st = ipc_transact(fake_ipc_port(), req, sizeof req,
                                   resp, sizeof resp, &resp_len, 1000U);

    TEST_ASSERT_EQUAL(IPC_OK, st);
    TEST_ASSERT_EQUAL_size_t(sizeof canned, resp_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(canned, resp, sizeof canned);
    TEST_ASSERT_EQUAL_INT(1, fake_ipc_notify_count());   /* rang the doorbell once */
    TEST_ASSERT_FALSE(fake_ipc_sema_held());             /* released on the way out */
}

void test_request_reaches_the_mailbox(void)
{
    uint8_t canned[4] = { 0x02U, 0x01U, 0x00U, 0x01U };
    fake_ipc_script_response(canned, sizeof canned);

    uint8_t req[3] = { 0x02U, 0x00U, 0x00U };
    uint8_t resp[16]; size_t resp_len = 0U;
    (void)ipc_transact(fake_ipc_port(), req, sizeof req, resp, sizeof resp, &resp_len, 1000U);

    size_t got_len = 0U;
    const uint8_t *got = fake_ipc_last_request(&got_len);
    TEST_ASSERT_TRUE(got_len >= sizeof req);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(req, got, sizeof req);
}

void test_no_response_times_out_and_releases_sema(void)
{
    fake_ipc_script_no_response();

    uint8_t req[3] = { 0x02U, 0x00U, 0x00U };
    uint8_t resp[16]; size_t resp_len = 99U;
    ipc_status_t st = ipc_transact(fake_ipc_port(), req, sizeof req,
                                   resp, sizeof resp, &resp_len, 100U);

    TEST_ASSERT_EQUAL(IPC_TIMEOUT, st);
    TEST_ASSERT_FALSE(fake_ipc_sema_held());   /* must not strand the semaphore */
}

void test_denied_semaphore_reports_busy(void)
{
    fake_ipc_script_deny_sema();

    uint8_t req[3] = { 0x02U, 0x00U, 0x00U };
    uint8_t resp[16]; size_t resp_len = 0U;
    ipc_status_t st = ipc_transact(fake_ipc_port(), req, sizeof req,
                                   resp, sizeof resp, &resp_len, 100U);

    TEST_ASSERT_EQUAL(IPC_BUSY, st);
    TEST_ASSERT_EQUAL_INT(0, fake_ipc_notify_count());   /* never got to notify */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_successful_transaction_returns_response);
    RUN_TEST(test_request_reaches_the_mailbox);
    RUN_TEST(test_no_response_times_out_and_releases_sema);
    RUN_TEST(test_denied_semaphore_reports_busy);
    return UNITY_END();
}
