/*
 * crypto_service.c — FBL-side verify client (ADR-0016 orchestration, ADR-0017).
 *
 * Build a VERIFY_IMAGE request, hand it to the transport, decode the verdict.
 * Every failure path — no port, unencodable request, transport busy/timeout/
 * malformed, undecodable reply, out-of-range verdict — collapses to
 * CRYPTO_VERDICT_ERROR, which the boot layer treats identically to INVALID
 * (stay in FBL, never jump — ADR-0016 D5). There is no path that hangs and no
 * path that returns VALID without a decoded VALID from the M0+.
 */
#include "crypto_service.h"
#include "crypto_msg.h"

static const ipc_port_if_t *g_port;

void crypto_service_init(const ipc_port_if_t *port)
{
    g_port = port;
}

crypto_verdict_t crypto_verify_image(uint32_t base, uint32_t len, uint32_t key_id)
{
    if (g_port == NULL)
    {
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_msg_t reqm;
    crypto_make_verify_request(base, len, key_id, &reqm);

    uint8_t req[CRYPTO_MSG_MAX_WIRE];
    size_t req_len = crypto_msg_encode(&reqm, req, sizeof req);
    if (req_len == 0U)
    {
        return CRYPTO_VERDICT_ERROR;
    }

    uint8_t resp[CRYPTO_MSG_MAX_WIRE];
    size_t resp_len = 0U;
    ipc_status_t st = ipc_transact(g_port, req, req_len,
                                   resp, sizeof resp, &resp_len,
                                   CRYPTO_VERIFY_TIMEOUT_MS);
    if (st != IPC_OK)
    {
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_msg_t respm;
    if (!crypto_msg_decode(resp, resp_len, &respm))
    {
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_verdict_t v = CRYPTO_VERDICT_ERROR;
    if (!crypto_parse_verdict(&respm, &v))
    {
        return CRYPTO_VERDICT_ERROR;
    }
    return v;
}
