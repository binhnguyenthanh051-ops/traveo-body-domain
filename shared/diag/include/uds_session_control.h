/*
 * uds_session_control.h — 0x10 DiagnosticSessionControl (ADR-0012 D1 layer 3,
 * ADR-0014 D1).
 *
 * Validates the requested session (default/programming) and reports it in
 * the positive response; uds_session.c applies the actual state transition
 * after a positive result (ADR-0012 design principle: handlers never mutate
 * session state directly, only session applies transitions).
 */
#ifndef UDS_SESSION_CONTROL_H
#define UDS_SESSION_CONTROL_H

#include "uds_handler.h"

/* The singleton handler instance (const -- no init needed, no state of its
 * own beyond what ctx/session already track). */
const uds_handler_if_t *uds_session_control_handler(void);

#endif /* UDS_SESSION_CONTROL_H */
