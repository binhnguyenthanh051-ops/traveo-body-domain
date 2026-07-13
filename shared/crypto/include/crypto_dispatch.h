/*
 * crypto_dispatch.h — M0+-side service dispatch (ADR-0017 D3, layer 2/3).
 *
 * Command pattern, exactly like uds_handler_if_t (ADR-0012 D3): one handler
 * per op behind a common interface, wired as a static config-time table
 * (ADR-0012 D3 — no runtime registration). A handler touches only the decoded
 * request and the response it writes; the real SHA-256 / ECDSA back end lives
 * below it (target-only, ADR-0017 layer 4), faked in host tests.
 *
 * The dispatcher ALWAYS produces a response (ADR-0017: "never silently
 * drops"): a malformed request, an unknown op (incl. the reserved-but-unbuilt
 * MAC 0x03), or a handler failure all become an ERROR verdict.
 */
#ifndef CRYPTO_DISPATCH_H
#define CRYPTO_DISPATCH_H

#include "crypto_types.h"

typedef struct {
    crypto_op_t op;
    /* Handle a decoded request, write a decoded response. Return false to
     * signal failure (the dispatcher turns that into an ERROR verdict). */
    bool (*handle)(const crypto_msg_t *req, crypto_msg_t *resp);
} crypto_handler_if_t;

/* Bind the handler table (author-ordered, static — ADR-0012 D3). The
 * composition root (the M0+ image, ADR-0017 D4) owns table/count. */
void crypto_dispatch_init(const crypto_handler_if_t *table, size_t count);

/* Decode req[0..req_len), dispatch on op, encode the response into resp
 * (capacity resp_cap, actual in *resp_len). Returns true if a response was
 * produced (including a synthesized ERROR verdict); false only if resp_cap is
 * too small to hold even a verdict. */
bool crypto_dispatch(const uint8_t *req, size_t req_len,
                     uint8_t *resp, size_t resp_cap, size_t *resp_len);

#endif /* CRYPTO_DISPATCH_H */
