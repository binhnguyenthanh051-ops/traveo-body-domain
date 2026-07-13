/*
 * crypto_dispatch.c — M0+-side service dispatch (ADR-0017 D3).
 *
 * Decode the request, route on op, encode the response. ALWAYS answers: a
 * malformed request, an unknown op (incl. the reserved-but-unbuilt MAC 0x03),
 * or a handler that returns false all become an ERROR verdict — never a silent
 * drop. The echo op is the request's op where known, else NONE.
 */
#include "crypto_dispatch.h"
#include "crypto_msg.h"

static const crypto_handler_if_t *g_table;
static size_t g_count;

void crypto_dispatch_init(const crypto_handler_if_t *table, size_t count)
{
    g_table = table;
    g_count = count;
}

static const crypto_handler_if_t *find_handler(uint8_t op)
{
    for (size_t i = 0U; i < g_count; ++i)
    {
        if ((uint8_t)g_table[i].op == op)
        {
            return &g_table[i];
        }
    }
    return NULL;
}

bool crypto_dispatch(const uint8_t *req, size_t req_len,
                     uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (resp_len != NULL)
    {
        *resp_len = 0U;
    }
    if (resp == NULL)
    {
        return false;
    }

    crypto_msg_t reqm;
    crypto_msg_t respm;
    crypto_op_t echo = CRYPTO_OP_NONE;
    bool answered = false;

    if ((req != NULL) && crypto_msg_decode(req, req_len, &reqm))
    {
        echo = (crypto_op_t)reqm.op_code;
        const crypto_handler_if_t *h = find_handler(reqm.op_code);
        if ((h != NULL) && (h->handle != NULL))
        {
            answered = h->handle(&reqm, &respm);
        }
    }

    if (!answered)
    {
        crypto_make_verdict(echo, CRYPTO_VERDICT_ERROR, &respm);
    }

    size_t n = crypto_msg_encode(&respm, resp, resp_cap);
    if (n == 0U)
    {
        return false;
    }
    if (resp_len != NULL)
    {
        *resp_len = n;
    }
    return true;
}
