/*
 * uds_ecu_reset.h — 0x11 ECUReset (ADR-0012 D1 layer 3).
 *
 * Deliberately has no dependency on any reset mechanism: handle() validates
 * the request and writes the positive response; uds_session.c reports
 * UDS_SESSION_EVENT_RESET_REQUESTED (uds_session.h) once that response has
 * been sent, and the composition root performs the actual, image-specific
 * reset sequence (the FBL's is ADR-0007 D10: clear .noinit, then
 * fbl_port_system_reset). This keeps the handler reusable by a future
 * App-side instance of this server, whose reset sequence differs.
 */
#ifndef UDS_ECU_RESET_H
#define UDS_ECU_RESET_H

#include "uds_handler.h"

const uds_handler_if_t *uds_ecu_reset_handler(void);

#endif /* UDS_ECU_RESET_H */
