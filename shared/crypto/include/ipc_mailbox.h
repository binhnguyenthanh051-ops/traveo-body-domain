/*
 * ipc_mailbox.h — cross-core IPC transport (ADR-0018, layer 1).
 *
 * The physical carrier between the CM4 (FBL) and CM0+ (crypto). Moves an
 * OPAQUE request/response buffer; knows nothing about the op it carries — the
 * cross-core analogue of ISO-TP handing up a complete buffer (ADR-0013).
 *
 * Synchronous, single-outstanding (ADR-0017 D2 / ADR-0018 D3): the FBL has
 * nothing to do at boot but wait for the verdict. The blocking wait is a
 * BOUNDED poll (response_ready + now_ms) so it stays host-testable with a fake
 * clock and a scripted responder — exactly the ADR-0008 dwell / ADR-0013 N_Cr
 * pattern. The transport REPORTS a timeout; it does not decide what a timeout
 * MEANS — that policy (⇒ fail-safe, ADR-0016 D5) belongs to the caller
 * (ADR-0018 D4).
 */
#ifndef IPC_MAILBOX_H
#define IPC_MAILBOX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Transaction outcome. IPC_OK means a response was received and copied out;
 * decoding it is the caller's concern (crypto_msg). Every non-OK outcome is a
 * reason the caller maps to ERROR ⇒ fail-safe (ADR-0016 D5). */
typedef enum {
    IPC_OK = 0,
    IPC_BUSY,       /* could not acquire the mailbox semaphore */
    IPC_TIMEOUT,    /* no response within timeout_ms (M0+ dead/busy) */
    IPC_MALFORMED   /* response present but not copyable (over-long) */
} ipc_status_t;

/* The hardware seam (ADR-0018 D1/D2), a pure vtable. Target binds the real
 * HW semaphore + IPC channel + notify + clock; host tests bind a fake.
 *
 * The transport is payload-agnostic, so the byte count travels with the
 * doorbell, not inside the buffer: notify() carries the request length (the
 * IPC channel register sits next to the pointer, ADR-0018 D1) and
 * response_len() reports how many bytes the M0+ wrote back. Neither side ever
 * parses the payload. */
typedef struct {
    bool     (*sema_try_acquire)(void);  /* non-blocking; true if ownership taken */
    void     (*sema_release)(void);
    uint8_t *(*mailbox)(void);           /* shared-RAM buffer, both cores map it */
    size_t     mailbox_cap;
    void     (*notify)(size_t req_len);  /* ring the M0+ doorbell with the request length */
    bool     (*response_ready)(void);    /* has the M0+ answered? (notify-set flag) */
    size_t   (*response_len)(void);      /* bytes the M0+ wrote; valid once response_ready() */
    uint32_t (*now_ms)(void);            /* fbl_port_now_ms on target */
} ipc_port_if_t;

/* Acquire → write req → notify → bounded-wait → read resp → release.
 * On success copies up to resp_cap response bytes and sets *resp_len.
 * timeout_ms bounds the wait; 0 means "one look, no waiting". */
ipc_status_t ipc_transact(const ipc_port_if_t *port,
                          const uint8_t *req, size_t req_len,
                          uint8_t *resp, size_t resp_cap, size_t *resp_len,
                          uint32_t timeout_ms);

#endif /* IPC_MAILBOX_H */
