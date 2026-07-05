/*
 * uds_ecu_reset.c — 0x11 ECUReset (ADR-0012).
 *
 * Accepts the request and echoes the subfunction. Never resets anything
 * itself -- uds_session.c reports UDS_SESSION_EVENT_RESET_REQUESTED once the
 * positive response has been sent; the composition root performs the actual
 * reset sequence (ADR-0007 D10 for the FBL).
 */
#include "uds_ecu_reset.h"

static uds_result_t handle(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 1U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }
    if (resp_cap < 1U) { return UDS_NRC_GENERAL_REJECT; }
    resp[0] = req[0];
    *resp_len = 1U;
    return UDS_NRC_NONE;
}

static const uds_handler_if_t g_if = {
    .sid = UDS_SID_ECU_RESET,
    .handle = handle
};

const uds_handler_if_t *uds_ecu_reset_handler(void) { return &g_if; }
