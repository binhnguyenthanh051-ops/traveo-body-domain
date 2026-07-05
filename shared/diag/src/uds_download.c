/*
 * uds_download.c — 0x34/0x36/0x37 download bookkeeping (ADR-0012 D4/D5).
 *
 * Per D5: requestTransferExit never reports success on an incomplete/gapped
 * transfer, and a failed exit resets to IDLE (cleanly re-startable, never
 * resumable). A wrong or skipped block-sequence counter is rejected before
 * anything is written to flash.
 */
#include "uds_download.h"

static const hal_flash_if_t *g_flash;
static uds_download_state_t g_state = UDS_DOWNLOAD_IDLE;
static uint32_t g_addr;
static uint32_t g_total_len;
static uint32_t g_written_len;
static uint8_t  g_next_seq;

void uds_download_init(const hal_flash_if_t *flash)
{
    g_flash = flash;
    g_state = UDS_DOWNLOAD_IDLE;
}

static uds_result_t request_download_handle(const uint8_t *req, size_t req_len,
                                             uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 8U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }

    g_addr = ((uint32_t)req[0] << 24) | ((uint32_t)req[1] << 16) |
             ((uint32_t)req[2] << 8)  |  (uint32_t)req[3];
    g_total_len = ((uint32_t)req[4] << 24) | ((uint32_t)req[5] << 16) |
                  ((uint32_t)req[6] << 8)  |  (uint32_t)req[7];
    g_written_len = 0U;
    g_next_seq = 1U;
    g_state = UDS_DOWNLOAD_ACTIVE;

    if (resp_cap < 2U) { return UDS_NRC_GENERAL_REJECT; }
    uint16_t max_block = 512U;
    resp[0] = (uint8_t)(max_block >> 8);
    resp[1] = (uint8_t)(max_block);
    *resp_len = 2U;
    return UDS_NRC_NONE;
}

static uds_result_t transfer_data_handle(const uint8_t *req, size_t req_len,
                                          uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (g_state != UDS_DOWNLOAD_ACTIVE) { return UDS_NRC_REQUEST_SEQUENCE_ERROR; }
    if (req_len < 1U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }

    uint8_t seq = req[0];
    if (seq != g_next_seq) { return UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER; }

    size_t data_len = req_len - 1U;
    if ((g_written_len + (uint32_t)data_len) > g_total_len)
    {
        return UDS_NRC_REQUEST_OUT_OF_RANGE;
    }
    if (g_flash->write(g_addr + g_written_len, &req[1], data_len) != 0)
    {
        return UDS_NRC_GENERAL_PROGRAMMING_FAILURE;
    }

    g_written_len += (uint32_t)data_len;
    /* ISO 14229 blockSequenceCounter: starts at 1, wraps 0xFF -> 0x00 and
     * continues from there (0 is a valid value after the first wrap). */
    g_next_seq = (uint8_t)(g_next_seq + 1U);

    if (resp_cap < 1U) { return UDS_NRC_GENERAL_REJECT; }
    resp[0] = seq;
    *resp_len = 1U;
    return UDS_NRC_NONE;
}

static uds_result_t request_transfer_exit_handle(const uint8_t *req, size_t req_len,
                                                  uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    (void)req;
    (void)req_len;
    (void)resp;
    (void)resp_cap;

    if (g_state != UDS_DOWNLOAD_ACTIVE) { return UDS_NRC_REQUEST_SEQUENCE_ERROR; }

    if (g_written_len != g_total_len)
    {
        g_state = UDS_DOWNLOAD_IDLE;   /* re-startable, not resumable -- ADR-0012 D5 */
        return UDS_NRC_REQUEST_SEQUENCE_ERROR;
    }

    g_state = UDS_DOWNLOAD_DONE;
    *resp_len = 0U;
    return UDS_NRC_NONE;
}

static const uds_handler_if_t g_request_download_if = { .sid = UDS_SID_REQUEST_DOWNLOAD, .handle = request_download_handle };
static const uds_handler_if_t g_transfer_data_if     = { .sid = UDS_SID_TRANSFER_DATA, .handle = transfer_data_handle };
static const uds_handler_if_t g_transfer_exit_if     = { .sid = UDS_SID_REQUEST_TRANSFER_EXIT, .handle = request_transfer_exit_handle };

const uds_handler_if_t *uds_request_download_handler(void)      { return &g_request_download_if; }
const uds_handler_if_t *uds_transfer_data_handler(void)         { return &g_transfer_data_if; }
const uds_handler_if_t *uds_request_transfer_exit_handler(void) { return &g_transfer_exit_if; }

uds_download_state_t uds_download_current_state(void) { return g_state; }
