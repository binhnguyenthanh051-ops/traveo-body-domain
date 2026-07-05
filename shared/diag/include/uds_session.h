/*
 * uds_session.h — the UDS session ("DCM"-like) layer (ADR-0012 D1 layer 2,
 * ADR-0014).
 *
 * Owns session-type state (D1) and SID dispatch; delegates security-access
 * state entirely to uds_security_access (queried here, never exposed to other
 * handlers directly -- ADR-0012 D1 "never sideways"). Depends downward on
 * isotp.h; depends on the handler interface (uds_handler_if_t), never on a
 * concrete handler module by name -- the dispatch table is supplied by the
 * composition root (ADR-0012 D3: static, config-time, no runtime
 * registration).
 */
#ifndef UDS_SESSION_H
#define UDS_SESSION_H

#include "uds_types.h"
#include "uds_handler.h"
#include <stdbool.h>

/* Bind the dispatch table (SID -> handler). table/count are owned by the
 * caller (composition root); uds_session only ever reads through the
 * pointers. Call once at composition time. */
void uds_session_init(const uds_handler_if_t *const *table, size_t count);

/* Result of one tick: NONE on the ordinary path; RESET_REQUESTED once an
 * ECUReset (0x11) has been accepted and its positive response sent -- the
 * composition root (not this module) then performs the actual image-specific
 * reset sequence (the FBL's is ADR-0007 D10: clear .noinit, system reset).
 * Keeping the reset action out of shared/diag keeps it reusable by a future
 * App-side instance of this same server, whose reset sequence differs. */
typedef enum {
    UDS_SESSION_EVENT_NONE = 0,
    UDS_SESSION_EVENT_RESET_REQUESTED
} uds_session_event_t;

/* Drive one iteration: polls isotp (isotp_poll/isotp_take_received
 * internally), dispatches a completed request, sends the response via
 * isotp_send, and advances P2/P2-star/S3 timers. Call once per FBL super-loop
 * iteration, after sysmgr reports the diag resource READY. */
uds_session_event_t uds_session_tick(uint32_t now_ms);

/* Session/security queries -- the only way a handler observes this state
 * (parameterless: one session per image, same singleton idiom as
 * fbl_port_*). uds_session_is_unlocked() delegates to
 * uds_security_access_is_unlocked() (uds_security_access.h) -- a vertical
 * dependency, not exposed to other handlers directly (ADR-0012 D1). */
uds_session_state_t  uds_session_current(void);
bool                  uds_session_is_unlocked(void);

#endif /* UDS_SESSION_H */
