/*
 * uds_security_access.c — 0x27 SecurityAccess (ADR-0012, ADR-0014 D2/D3).
 *
 * Locked -> seed-sent -> unlocked. Seed is a deterministic counter (not a
 * TRNG); key = seed ^ UDS_SECURITY_KEY_XOR_CONST -- the honesty note in
 * uds_security_access.h applies: mechanism, not security.
 */
#include "uds_security_access.h"

static uds_security_state_t g_state = UDS_SEC_LOCKED;
static uint32_t g_seed;
static uint32_t g_seed_counter = 1U;

static uds_result_t handle(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 1U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }
    uint8_t subfunction = req[0];

    if (subfunction == 0x01U)   /* requestSeed */
    {
        g_seed = g_seed_counter;
        ++g_seed_counter;
        g_state = UDS_SEC_SEED_SENT;

        if (resp_cap < 5U) { return UDS_NRC_GENERAL_REJECT; }
        resp[0] = 0x01U;
        resp[1] = (uint8_t)(g_seed >> 24);
        resp[2] = (uint8_t)(g_seed >> 16);
        resp[3] = (uint8_t)(g_seed >> 8);
        resp[4] = (uint8_t)(g_seed);
        *resp_len = 5U;
        return UDS_NRC_NONE;
    }

    if (subfunction == 0x02U)   /* sendKey */
    {
        if (g_state != UDS_SEC_SEED_SENT)
        {
            g_state = UDS_SEC_LOCKED;
            return UDS_NRC_REQUEST_SEQUENCE_ERROR;
        }
        if (req_len < 5U)
        {
            g_state = UDS_SEC_LOCKED;
            return UDS_NRC_INVALID_KEY;
        }

        uint32_t key = ((uint32_t)req[1] << 24) | ((uint32_t)req[2] << 16) |
                       ((uint32_t)req[3] << 8)  |  (uint32_t)req[4];
        uint32_t expected = g_seed ^ UDS_SECURITY_KEY_XOR_CONST;

        if (key != expected)
        {
            g_state = UDS_SEC_LOCKED;
            return UDS_NRC_INVALID_KEY;
        }

        g_state = UDS_SEC_UNLOCKED;
        if (resp_cap < 1U) { return UDS_NRC_GENERAL_REJECT; }
        resp[0] = 0x02U;
        *resp_len = 1U;
        return UDS_NRC_NONE;
    }

    return UDS_NRC_REQUEST_OUT_OF_RANGE;
}

static const uds_handler_if_t g_if = {
    .sid = UDS_SID_SECURITY_ACCESS,
    .handle = handle
};

const uds_handler_if_t *uds_security_access_handler(void) { return &g_if; }
bool uds_security_access_is_unlocked(void) { return g_state == UDS_SEC_UNLOCKED; }
void uds_security_access_relock(void) { g_state = UDS_SEC_LOCKED; }
