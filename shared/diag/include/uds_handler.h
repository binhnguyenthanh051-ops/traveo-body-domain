/*
 * uds_handler.h — the service-handler interface (ADR-0012 D1 layer 3, D3).
 *
 * Command pattern: one handler per SID behind this common interface. A
 * handler touches only (req, resp) plus session/security queries exposed by
 * uds_session.h -- never CAN, never ISO-TP, never another handler's header
 * (ADR-0012 D1: never sideways; querying uds_session.h is a vertical
 * dependency on the session layer, not on another handler).
 */
#ifndef UDS_HANDLER_H
#define UDS_HANDLER_H

#include "uds_types.h"

/* handle() writes a positive response into resp (capacity resp_cap, actual
 * length in *resp_len) and returns UDS_NRC_NONE, or returns a negative
 * response code and leaves *resp_len untouched (the session layer builds the
 * 0x7F frame). req/req_len exclude the SID byte -- the session layer already
 * dispatched on it. */
typedef struct {
    uint8_t sid;
    uds_result_t (*handle)(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len);
} uds_handler_if_t;

#endif /* UDS_HANDLER_H */
