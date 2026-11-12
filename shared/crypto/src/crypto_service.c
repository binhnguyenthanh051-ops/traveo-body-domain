/*
 * crypto_service.c — FBL-side verify client (ADR-0016 orchestration, ADR-0017).
 *
 * Build a VERIFY_IMAGE request, hand it to the transport, decode the verdict.
 * Every failure path — no port, unencodable request, transport busy/timeout/
 * malformed, undecodable reply, a verdict for another op, out-of-range verdict —
 * collapses to
 * CRYPTO_VERDICT_ERROR, which the boot layer treats identically to INVALID
 * (stay in FBL, never jump — ADR-0016 D5). There is no path that hangs and no
 * path that returns VALID without a decoded VALID from the M0+.
 *
 * Every failure also emits exactly one LOG_EVT_CRYPTO_ERROR(op, crypto_fail_t),
 * so a dead or refusing M0+ is visible from the normal image (F-018). The
 * client is the one place every crypto op passes through on all three images
 * (Node A FBL + app, Node B app), which is why the event lives here and not in
 * each node's SecOC wiring.
 */
#include "crypto_service.h"
#include "crypto_msg.h"
#include "tb_log.h"
#include "log_events.h"

static const ipc_port_if_t *g_port;

/* One failure, one event. Returns nothing so the call sites stay one line. */
static void crypto_report(crypto_op_t op, crypto_fail_t reason)
{
    log_evt(LOG_EVT_CRYPTO_ERROR, (uint32_t)op, (uint16_t)reason);
}

/* The transport outcome as a failure reason. An explicit switch, not a cast
 * between enum types (MISRA C:2012 10.5); the values match anyway. */
static crypto_fail_t crypto_fail_from_ipc(ipc_status_t st)
{
    crypto_fail_t reason = CRYPTO_FAIL_IPC_MALFORMED;
    switch (st)
    {
        case IPC_BUSY:    reason = CRYPTO_FAIL_IPC_BUSY;    break;
        case IPC_TIMEOUT: reason = CRYPTO_FAIL_IPC_TIMEOUT; break;
        default:          reason = CRYPTO_FAIL_IPC_MALFORMED; break;
    }
    return reason;
}

void crypto_service_init(const ipc_port_if_t *port)
{
    g_port = port;
}

crypto_verdict_t crypto_verify_image(uint32_t base, uint32_t len, uint32_t key_id)
{
    if (g_port == NULL)
    {
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_REQUEST);
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_msg_t reqm;
    crypto_make_verify_request(base, len, key_id, &reqm);

    uint8_t req[CRYPTO_MSG_MAX_WIRE];
    size_t req_len = crypto_msg_encode(&reqm, req, sizeof req);
    if (req_len == 0U)
    {
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_REQUEST);
        return CRYPTO_VERDICT_ERROR;
    }

    uint8_t resp[CRYPTO_MSG_MAX_WIRE];
    size_t resp_len = 0U;
    ipc_status_t st = ipc_transact(g_port, req, req_len,
                                   resp, sizeof resp, &resp_len,
                                   CRYPTO_VERIFY_TIMEOUT_MS);
    if (st != IPC_OK)
    {
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, crypto_fail_from_ipc(st));
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_msg_t respm;
    if (!crypto_msg_decode(resp, resp_len, &respm))
    {
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_BAD_REPLY);
        return CRYPTO_VERDICT_ERROR;
    }

    crypto_verdict_t v = CRYPTO_VERDICT_ERROR;
    if (!crypto_parse_verdict(&respm, CRYPTO_OP_VERIFY_IMAGE, &v))
    {
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_BAD_REPLY);
        return CRYPTO_VERDICT_ERROR;
    }
    if (v == CRYPTO_VERDICT_ERROR)
    {
        /* INVALID is an answer (a bad signature) and is not reported here: a
         * tampered image must not look like a dead M0+. */
        crypto_report(CRYPTO_OP_VERIFY_IMAGE, CRYPTO_FAIL_REFUSED);
    }
    return v;
}

/* @impl ADR-0021 D7 : app-side MAC client (mirrors crypto_verify_image) */
bool crypto_mac(uint32_t key_id, const uint8_t *msg, size_t msg_len,
                uint8_t tag[CRYPTO_CMAC_TAG_LEN])
{
    if ((g_port == NULL) || (tag == NULL) || ((msg == NULL) && (msg_len != 0U)))
    {
        crypto_report(CRYPTO_OP_MAC, CRYPTO_FAIL_REQUEST);
        return false;
    }
    /* The request is key_id(4) + len(2) + msg; it must fit CRYPTO_MAX_PAYLOAD. */
    if (msg_len > (size_t)(CRYPTO_MAX_PAYLOAD - 6U))
    {
        crypto_report(CRYPTO_OP_MAC, CRYPTO_FAIL_REQUEST);
        return false;
    }

    crypto_msg_t reqm;
    crypto_make_mac_request(key_id, msg, (uint16_t)msg_len, &reqm);
    if (reqm.length == 0U)
    {
        crypto_report(CRYPTO_OP_MAC, CRYPTO_FAIL_REQUEST);
        return false;                   /* make_mac_request refused (overflow guard) */
    }

    uint8_t req[CRYPTO_MSG_MAX_WIRE];
    size_t req_len = crypto_msg_encode(&reqm, req, sizeof req);
    if (req_len == 0U)
    {
        crypto_report(CRYPTO_OP_MAC, CRYPTO_FAIL_REQUEST);
        return false;
    }

    uint8_t resp[CRYPTO_MSG_MAX_WIRE];
    size_t resp_len = 0U;
    ipc_status_t st = ipc_transact(g_port, req, req_len,
                                   resp, sizeof resp, &resp_len,
                                   CRYPTO_MAC_TIMEOUT_MS);
    if (st != IPC_OK)
    {
        crypto_report(CRYPTO_OP_MAC, crypto_fail_from_ipc(st));
        return false;
    }

    crypto_msg_t respm;
    if (!crypto_msg_decode(resp, resp_len, &respm))
    {
        crypto_report(CRYPTO_OP_MAC, CRYPTO_FAIL_BAD_REPLY);
        return false;
    }

    /* A tag response is 16 bytes; an M0+ error is a 1-byte verdict, which
     * crypto_parse_mac_response rejects ⇒ false ⇒ caller drops (fail-safe).
     * The verdict check only picks the reason: a verdict means the M0+ refused
     * (unknown key_id, crypto fault), anything else is a garbled reply. */
    if (!crypto_parse_mac_response(&respm, tag))
    {
        crypto_verdict_t v = CRYPTO_VERDICT_ERROR;
        crypto_report(CRYPTO_OP_MAC,
                      crypto_parse_verdict(&respm, CRYPTO_OP_MAC, &v)
                          ? CRYPTO_FAIL_REFUSED
                          : CRYPTO_FAIL_BAD_REPLY);
        return false;
    }
    return true;
}
