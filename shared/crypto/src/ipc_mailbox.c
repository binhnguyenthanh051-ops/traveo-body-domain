/*
 * ipc_mailbox.c — cross-core IPC transport (ADR-0018).
 *
 * acquire → write req → notify(len) → bounded-wait → read resp → release.
 * Payload-agnostic: it moves req_len bytes and returns response_len() bytes,
 * never parsing either. The wait is a bounded poll over response_ready() +
 * now_ms() (ADR-0018 D4) — host-testable with a fake clock. The transport
 * REPORTS a timeout; the caller decides it means fail-safe (ADR-0016 D5).
 */
#include "ipc_mailbox.h"

ipc_status_t ipc_transact(const ipc_port_if_t *port,
                          const uint8_t *req, size_t req_len,
                          uint8_t *resp, size_t resp_cap, size_t *resp_len,
                          uint32_t timeout_ms)
{
    if (resp_len != NULL)
    {
        *resp_len = 0U;
    }
    if ((port == NULL) || (req == NULL) || (resp == NULL))
    {
        return IPC_MALFORMED;
    }
    if (req_len > port->mailbox_cap)
    {
        return IPC_MALFORMED;
    }

    if (!port->sema_try_acquire())
    {
        return IPC_BUSY;
    }

    uint8_t *mb = port->mailbox();
    for (size_t i = 0U; i < req_len; ++i)
    {
        mb[i] = req[i];
    }
    port->notify(req_len);

    uint32_t start = port->now_ms();
    while (!port->response_ready())
    {
        uint32_t now = port->now_ms();
        /* now_ms() is a running clock, so `now` and `start` are samples taken at
         * different times and differ. cppcheck models now_ms() as pure (now==start)
         * and raises two false positives on the next line — the bogus zero then
         * also trips its unsigned-<-zero check. Suppress both; the unsigned
         * (now - start) wrap-safe compare is intended. */
        /* cppcheck-suppress duplicateExpression */
        /* cppcheck-suppress unsignedLessThanZero */
        if ((now - start) >= timeout_ms)
        {
            port->sema_release();
            return IPC_TIMEOUT;
        }
    }

    size_t n = port->response_len();
    if ((n > resp_cap) || (n > port->mailbox_cap))
    {
        port->sema_release();
        return IPC_MALFORMED;
    }

    mb = port->mailbox();
    for (size_t i = 0U; i < n; ++i)
    {
        resp[i] = mb[i];
    }
    if (resp_len != NULL)
    {
        *resp_len = n;
    }
    port->sema_release();
    return IPC_OK;
}
