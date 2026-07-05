/*
 * uds_types.h — pure UDS types and config, no functions (ADR-0012, ADR-0014).
 *
 * Shared by the session layer and every handler. SIDs and NRCs are the subset
 * ISO 14229 subset M3's download flow needs -- not a full UDS service table.
 */
#ifndef UDS_TYPES_H
#define UDS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------
 * Service IDs (ADR-0012 D1 layer 3)
 * ----------------------------------------------------------------- */
#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL   0x10U
#define UDS_SID_ECU_RESET                    0x11U
#define UDS_SID_SECURITY_ACCESS              0x27U
#define UDS_SID_ROUTINE_CONTROL              0x31U
#define UDS_SID_REQUEST_DOWNLOAD             0x34U
#define UDS_SID_TRANSFER_DATA                0x36U
#define UDS_SID_REQUEST_TRANSFER_EXIT        0x37U

#define UDS_SID_NEGATIVE_RESPONSE            0x7FU

/* -------------------------------------------------------------------
 * Negative response codes (subset relevant to M3)
 * ----------------------------------------------------------------- */
typedef enum {
    UDS_NRC_NONE                      = 0x00,  /* positive response */
    UDS_NRC_GENERAL_REJECT             = 0x10,
    UDS_NRC_SERVICE_NOT_SUPPORTED      = 0x11,
    UDS_NRC_CONDITIONS_NOT_CORRECT     = 0x22,
    UDS_NRC_REQUEST_SEQUENCE_ERROR     = 0x24,
    UDS_NRC_REQUEST_OUT_OF_RANGE       = 0x31,
    UDS_NRC_SECURITY_ACCESS_DENIED     = 0x33,
    UDS_NRC_INVALID_KEY                = 0x35,
    UDS_NRC_TRANSFER_DATA_SUSPENDED    = 0x71,
    UDS_NRC_GENERAL_PROGRAMMING_FAILURE= 0x72,
    UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER = 0x73,
    UDS_NRC_RESPONSE_PENDING           = 0x78
} uds_nrc_t;

/* A handler's outcome: NONE = positive response already written to resp;
 * anything else = negative response, session layer builds the 0x7F frame. */
typedef uds_nrc_t uds_result_t;

/* -------------------------------------------------------------------
 * Session-type state (ADR-0014 D1) -- owned by uds_session, not a handler.
 * ----------------------------------------------------------------- */
typedef enum {
    UDS_SESS_DEFAULT = 0,
    UDS_SESS_PROGRAMMING
} uds_session_state_t;

/* -------------------------------------------------------------------
 * Security-access state (ADR-0014 D2) -- owned by uds_security_access, not
 * exposed directly to other handlers (ADR-0012 D1: never sideways). Other
 * handlers query it only via uds_session_is_unlocked() (uds_session.h), a
 * vertical dependency on the session layer, never on uds_security_access.h
 * itself.
 *
 * Note: an earlier draft of this header threaded an opaque
 * `uds_session_ctx_t` through every handle() call for this purpose. Dropped
 * as unnecessary indirection -- there is exactly one session per image
 * (singleton, same as the fbl_port_* pattern), so a parameterless query is
 * simpler and equally testable, and still keeps the dependency vertical
 * (session layer's own header), not sideways.
 * ----------------------------------------------------------------- */
typedef enum {
    UDS_SEC_LOCKED = 0,
    UDS_SEC_SEED_SENT,
    UDS_SEC_UNLOCKED
} uds_security_state_t;

/* -------------------------------------------------------------------
 * Timing (ADR-0013 D3 / ADR-0014 D4), milliseconds.
 * ----------------------------------------------------------------- */
#ifndef UDS_P2_SERVER_MAX_MS
#define UDS_P2_SERVER_MAX_MS    50U
#endif
#ifndef UDS_P2_STAR_SERVER_MAX_MS
#define UDS_P2_STAR_SERVER_MAX_MS  5000U
#endif
#ifndef UDS_S3_SERVER_MS
#define UDS_S3_SERVER_MS        5000U
#endif

#endif /* UDS_TYPES_H */
