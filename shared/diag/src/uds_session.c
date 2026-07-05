/*
 * uds_session.c — the UDS session layer (ADR-0012 D1 layer 2, ADR-0014).
 *
 * Dispatches over the bound table, applies the session-type transition after
 * a positive 0x10, relocks security on an S3 timeout or session drop, and
 * reports ECUReset acceptance for the composition root to act on.
 */
#include "uds_session.h"
#include "isotp.h"
#include "isotp_types.h"
#include "uds_security_access.h"
#include <string.h>

static const uds_handler_if_t *const *g_table;
static size_t g_table_count;
static uds_session_state_t g_session_state;
static uint32_t g_last_activity_ms;

static uint8_t g_req_buf[ISOTP_MAX_PAYLOAD];
static uint8_t g_resp_buf[ISOTP_MAX_PAYLOAD];

void uds_session_init(const uds_handler_if_t *const *table, size_t count)
{
    g_table = table;
    g_table_count = count;
    g_session_state = UDS_SESS_DEFAULT;
    g_last_activity_ms = 0U;
}

static const uds_handler_if_t *find_handler(uint8_t sid)
{
    for (size_t i = 0U; i < g_table_count; ++i)
    {
        if (g_table[i]->sid == sid) { return g_table[i]; }
    }
    return NULL;
}

static void send_negative(uint8_t sid, uds_nrc_t nrc)
{
    g_resp_buf[0] = UDS_SID_NEGATIVE_RESPONSE;
    g_resp_buf[1] = sid;
    g_resp_buf[2] = (uint8_t)nrc;
    (void)isotp_send(g_resp_buf, 3U);
}

uds_session_event_t uds_session_tick(uint32_t now_ms)
{
    isotp_poll(now_ms);

    if ((g_session_state != UDS_SESS_DEFAULT) &&
        ((now_ms - g_last_activity_ms) >= UDS_S3_SERVER_MS))
    {
        g_session_state = UDS_SESS_DEFAULT;
        uds_security_access_relock();
    }

    size_t req_len = 0U;
    if (!isotp_take_received(g_req_buf, sizeof g_req_buf, &req_len))
    {
        return UDS_SESSION_EVENT_NONE;
    }
    if (req_len < 1U)
    {
        return UDS_SESSION_EVENT_NONE;
    }

    uint8_t sid = g_req_buf[0];
    const uds_handler_if_t *h = find_handler(sid);
    if (h == NULL)
    {
        send_negative(sid, UDS_NRC_SERVICE_NOT_SUPPORTED);
        return UDS_SESSION_EVENT_NONE;
    }

    size_t body_len = 0U;
    uds_result_t r = h->handle(&g_req_buf[1], req_len - 1U,
                                &g_resp_buf[1], sizeof(g_resp_buf) - 1U, &body_len);
    if (r != UDS_NRC_NONE)
    {
        send_negative(sid, r);
        return UDS_SESSION_EVENT_NONE;
    }

    g_resp_buf[0] = sid;
    (void)isotp_send(g_resp_buf, 1U + body_len);
    g_last_activity_ms = now_ms;

    uds_session_event_t event = UDS_SESSION_EVENT_NONE;
    if (sid == UDS_SID_DIAGNOSTIC_SESSION_CONTROL)
    {
        uint8_t subfunction = g_resp_buf[1];
        if (subfunction == 0x01U)
        {
            g_session_state = UDS_SESS_DEFAULT;
            uds_security_access_relock();
        }
        else if (subfunction == 0x02U)
        {
            g_session_state = UDS_SESS_PROGRAMMING;
        }
        else
        {
            /* unreachable: uds_session_control rejects any other value */
        }
    }
    else if (sid == UDS_SID_ECU_RESET)
    {
        event = UDS_SESSION_EVENT_RESET_REQUESTED;
    }
    else
    {
        /* no session-level follow-up for this SID */
    }

    return event;
}

uds_session_state_t uds_session_current(void) { return g_session_state; }
bool uds_session_is_unlocked(void) { return uds_security_access_is_unlocked(); }
