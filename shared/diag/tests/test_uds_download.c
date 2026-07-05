/*
 * test_uds_download.c — Unity tests for 0x34/0x36/0x37 download bookkeeping
 * (ADR-0012 D4) and the interrupted-transfer safety property (D5): a gap,
 * an out-of-order block, or an incomplete transfer must never let
 * requestTransferExit report success, and a failed/aborted attempt must
 * leave the state machine cleanly re-startable, never half-committed.
 *
 * Fails against the current stub uds_download.c (step 5 implements). Test
 * harness — exempt from MISRA.
 */
#include "unity.h"
#include "uds_download.h"
#include <string.h>

extern void fake_flash_reset(void);
extern const uint8_t *fake_flash_buffer(void);
extern const hal_flash_if_t *fake_flash_hal(void);

static void req_download(uint32_t addr, uint32_t size, uint8_t *resp, size_t *resp_len)
{
    uint8_t req[8];
    req[0] = (uint8_t)(addr >> 24); req[1] = (uint8_t)(addr >> 16);
    req[2] = (uint8_t)(addr >> 8);  req[3] = (uint8_t)(addr);
    req[4] = (uint8_t)(size >> 24); req[5] = (uint8_t)(size >> 16);
    req[6] = (uint8_t)(size >> 8);  req[7] = (uint8_t)(size);
    uds_request_download_handler()->handle(req, sizeof req, resp, 64U, resp_len);
}

static uds_result_t xfer(uint8_t seq, const uint8_t *data, size_t data_len,
                          uint8_t *resp, size_t *resp_len)
{
    uint8_t req[1 + 64];
    req[0] = seq;
    memcpy(&req[1], data, data_len);
    return uds_transfer_data_handler()->handle(req, 1U + data_len, resp, sizeof req, resp_len);
}

static uds_result_t exit_transfer(uint8_t *resp, size_t *resp_len)
{
    return uds_request_transfer_exit_handler()->handle(NULL, 0U, resp, 8U, resp_len);
}

void setUp(void)
{
    fake_flash_reset();
    uds_download_init(fake_flash_hal());
}
void tearDown(void) {}

void test_idle_at_startup(void)
{
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_IDLE, uds_download_current_state());
}

void test_request_download_starts_active(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 100U, resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_ACTIVE, uds_download_current_state());
}

void test_transfer_data_writes_flash_in_sequence(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 20U, resp, &resp_len);

    uint8_t block[20];
    memset(block, 0x42, sizeof block);
    uds_result_t r = xfer(1U, block, sizeof block, resp, &resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block, fake_flash_buffer(), sizeof block);
}

void test_transfer_data_without_active_download_is_sequence_error(void)
{
    uint8_t block[4] = { 1, 2, 3, 4 };
    uint8_t resp[8]; size_t resp_len = 0U;
    uds_result_t r = xfer(1U, block, sizeof block, resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_NRC_REQUEST_SEQUENCE_ERROR, r);
}

/* ==================================================================
 * Interrupted-transfer safety (ADR-0012 D5) -- the core of this file
 * ================================================================ */

void test_wrong_first_sequence_rejected_and_flash_untouched(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 20U, resp, &resp_len);

    uint8_t block[20];
    memset(block, 0x42, sizeof block);
    uds_result_t r = xfer(2U, block, sizeof block, resp, &resp_len);   /* must start at 1 */

    TEST_ASSERT_EQUAL(UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER, r);
    uint8_t erased[20];
    memset(erased, 0xFF, sizeof erased);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(erased, fake_flash_buffer(), sizeof erased);   /* not written */
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_ACTIVE, uds_download_current_state());       /* not corrupted */
}

void test_sequence_gap_rejected_and_second_block_not_written(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 40U, resp, &resp_len);

    uint8_t block1[20]; memset(block1, 0x11, sizeof block1);
    xfer(1U, block1, sizeof block1, resp, &resp_len);

    uint8_t block3[20]; memset(block3, 0x33, sizeof block3);
    uds_result_t r = xfer(3U, block3, sizeof block3, resp, &resp_len);   /* skips 2 */

    TEST_ASSERT_EQUAL(UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER, r);
    uint8_t erased[20]; memset(erased, 0xFF, sizeof erased);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(erased, fake_flash_buffer() + 20, sizeof erased); /* block 3 not written */
}

void test_transfer_exit_on_incomplete_data_fails_and_resets_to_idle(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 100U, resp, &resp_len);         /* declares 100 bytes */

    uint8_t block[20]; memset(block, 0xAA, sizeof block);
    xfer(1U, block, sizeof block, resp, &resp_len);  /* only 20 of 100 written */

    uds_result_t r = exit_transfer(resp, &resp_len);

    TEST_ASSERT_NOT_EQUAL(UDS_NRC_NONE, r);
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_IDLE, uds_download_current_state());  /* re-startable, not resumable */
}

void test_restart_after_aborted_transfer_is_clean(void)
{
    /* First attempt: interrupted, aborted (mirrors the test above). */
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 100U, resp, &resp_len);
    uint8_t block[20]; memset(block, 0xAA, sizeof block);
    xfer(1U, block, sizeof block, resp, &resp_len);
    exit_transfer(resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_IDLE, uds_download_current_state());

    /* A fresh requestDownload must not inherit the old sequence counter or
     * size -- it starts clean, as if the aborted attempt never happened. */
    fake_flash_reset();
    req_download(0U, 10U, resp, &resp_len);
    uint8_t fresh[10]; memset(fresh, 0x55, sizeof fresh);
    uds_result_t r = xfer(1U, fresh, sizeof fresh, resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);

    uds_result_t exit_r = exit_transfer(resp, &resp_len);
    TEST_ASSERT_EQUAL(UDS_NRC_NONE, exit_r);
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_DONE, uds_download_current_state());
}

void test_transfer_exit_success_when_complete(void)
{
    uint8_t resp[8]; size_t resp_len = 0U;
    req_download(0U, 20U, resp, &resp_len);
    uint8_t block[20]; memset(block, 0x77, sizeof block);
    xfer(1U, block, sizeof block, resp, &resp_len);

    uds_result_t r = exit_transfer(resp, &resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
    TEST_ASSERT_EQUAL(UDS_DOWNLOAD_DONE, uds_download_current_state());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_idle_at_startup);
    RUN_TEST(test_request_download_starts_active);
    RUN_TEST(test_transfer_data_writes_flash_in_sequence);
    RUN_TEST(test_transfer_data_without_active_download_is_sequence_error);

    RUN_TEST(test_wrong_first_sequence_rejected_and_flash_untouched);
    RUN_TEST(test_sequence_gap_rejected_and_second_block_not_written);
    RUN_TEST(test_transfer_exit_on_incomplete_data_fails_and_resets_to_idle);
    RUN_TEST(test_restart_after_aborted_transfer_is_clean);
    RUN_TEST(test_transfer_exit_success_when_complete);

    return UNITY_END();
}
