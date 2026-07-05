/*
 * uds_session_control.c — 0x10 DiagnosticSessionControl (ADR-0012, ADR-0014
 * D1).
 *
 * Validates subfunction 0x01 (default) / 0x02 (programming) and echoes it;
 * uds_session.c interprets a positive result and applies the actual
 * transition (handlers never mutate session state directly).
 */
#include "uds_session_control.h"

static uds_result_t handle(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 1U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }
    uint8_t subfunction = req[0];
    if ((subfunction != 0x01U) && (subfunction != 0x02U))
    {
        return UDS_NRC_REQUEST_OUT_OF_RANGE;
    }
    if (resp_cap < 1U) { return UDS_NRC_GENERAL_REJECT; }
    resp[0] = subfunction;
    *resp_len = 1U;
    return UDS_NRC_NONE;
}

static const uds_handler_if_t g_if = {
    .sid = UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
    .handle = handle
};

const uds_handler_if_t *uds_session_control_handler(void) { return &g_if; }
