/*
 * secoc_app.c — Node A (gateway) SecOC application glue (M5 seam 3). See header.
 * Mirror of Node B's secoc_app with sender/receiver roles swapped.
 *
 * @impl ADR-0021 D7 : SecOC oracle bound to the M0+ crypto_mac client
 * @impl REQ-SECOC-001 : outgoing commands secured; incoming telemetry verified
 * @impl REQ-SECOC-008 : adopt the receiver floor from FRESHNESS_SYNC
 */
#include "secoc_app.h"
#include "secoc.h"
#include "secoc_freshness.h"
#include "body_msgs.h"         /* body_msg_pdu_len, MSG_ID_* */
#include "secoc_rx.h"         /* the composed RX verdict + SecOC events (ADR-0023 D11) */
#include "secoc_key_id.h"     /* SECOC_MAC_KEY_ID (public selector) */
#include "crypto_service.h"   /* crypto_mac */
#include "crypto_types.h"

secoc_rx_t g_secoc_rx;
volatile uint32_t g_secoc_drop_decode;

static const secoc_mac_if_t g_oracle = { crypto_mac };
static const secoc_crypto_t g_cy = { &g_oracle, SECOC_MAC_KEY_ID };

/* RAM freshness store (M5; eeprom_emu in M6 — same honest caveat as Node B). */
enum { SECOC_DOM_TX_SELF = 0u, SECOC_DOM_RX_B = 1u, SECOC_STORE_DOMAINS = 4u };

static struct {
    bool     valid;
    uint16_t epoch;
} s_store[SECOC_STORE_DOMAINS];

static bool store_load(uint8_t domain, uint16_t *epoch)
{
    if ((domain >= (uint8_t)SECOC_STORE_DOMAINS) || !s_store[domain].valid)
    {
        return false;
    }
    *epoch = s_store[domain].epoch;
    return true;
}

static bool store_commit(uint8_t domain, uint16_t epoch)
{
    if (domain >= (uint8_t)SECOC_STORE_DOMAINS)
    {
        return false;
    }
    s_store[domain].valid = true;
    s_store[domain].epoch = epoch;
    return true;
}

static const secoc_freshness_store_if_t g_store = { store_load, store_commit };

/* Gateway SENDS commands 0x120/0x121 (one sender epoch); RECEIVES B's telemetry
 * 0x200 + sync 0x2F0 (one receiver context tracking B's sender epoch). */
static secoc_tx_ctx_t g_tx;
static secoc_rx_ctx_t g_rx;

static const uint16_t g_tx_ids[] = { MSG_ID_DOOR_CMD, MSG_ID_LIGHT_CMD };
static const uint16_t g_rx_ids[] = { MSG_ID_SENSOR_RPT, MSG_ID_FRESHNESS_SYNC };

void secoc_app_init(void)
{
    for (uint32_t i = 0u; i < (uint32_t)SECOC_STORE_DOMAINS; ++i)
    {
        s_store[i].valid = false;
        s_store[i].epoch = 0u;
    }
    (void)secoc_tx_boot(&g_tx, &g_store, (uint8_t)SECOC_DOM_TX_SELF,
                        g_tx_ids, sizeof g_tx_ids / sizeof g_tx_ids[0]);
    (void)secoc_rx_boot(&g_rx, &g_store, (uint8_t)SECOC_DOM_RX_B,
                        g_rx_ids, sizeof g_rx_ids / sizeof g_rx_ids[0]);
    secoc_rx_init(&g_secoc_rx, &g_cy, &g_rx);
    /* Per-ID PDU lengths, so a CAN FD-padded secured frame verifies (W40 bench). */
    secoc_rx_set_pdu_len_of(&g_secoc_rx, body_msg_pdu_len);
}

/* Verify a received secured frame on `can_id`; on VALID+fresh, hand back the
 * stripped PDU (in `pdu`, `*pdu_len`). Shared by telemetry + sync RX.
 *
 * One line, because the verdict, the per-reason counters and the contract
 * events all live in the seam now (ADR-0023 D11) — this used to be the second
 * hand-maintained copy of that sequence. */
static bool rx_authentic(const can_raw_frame_t *f, uint8_t *pdu, size_t cap,
                         size_t *pdu_len)
{
    return (secoc_rx_process(&g_secoc_rx, (uint16_t)f->id, f->data, f->len,
                             pdu, cap, pdu_len) == SECOC_RX_ACCEPT);
}

bool secoc_app_verify_telemetry(const can_raw_frame_t *f, body_msg_t *out)
{
    if ((f == NULL) || (out == NULL))
    {
        return false;
    }
    uint8_t pdu[CAN_MAX_DLEN];
    size_t  pdu_len = 0u;
    if (!rx_authentic(f, pdu, sizeof pdu, &pdu_len))
    {
        return false;
    }
    if (body_decode(f->id, pdu, pdu_len, out) != 1)
    {
        ++g_secoc_drop_decode;
        return false;
    }
    return true;
}

bool secoc_app_handle_sync(const can_raw_frame_t *f)
{
    if (f == NULL)
    {
        return false;
    }
    uint8_t pdu[CAN_MAX_DLEN];
    size_t  pdu_len = 0u;
    if (!rx_authentic(f, pdu, sizeof pdu, &pdu_len))
    {
        return false;
    }
    if (pdu_len < 2u)
    {
        ++g_secoc_drop_decode;
        return false;
    }
    /* PDU = the receiver's requested floor (u16 LE); adopt it into our command
     * epoch so B accepts our commands again after its reboot (D5).
     *
     * This is where the resync COMPLETES, which is why LOG_EVT_SECOC_RESYNC is
     * emitted on the gateway and not on the actuator: B only sends the sync.
     * secoc_rx_sync_adopt() reports only when the epoch actually moved. */
    uint16_t floor = (uint16_t)((uint16_t)pdu[0] | ((uint16_t)pdu[1] << 8));
    (void)secoc_rx_sync_adopt(&g_tx, (uint16_t)f->id, floor);
    return true;
}

/* Build a secured frame for `can_id` from a packed PDU. */
static bool build_secured(uint16_t can_id, const uint8_t *pdu, size_t pdu_len,
                          can_raw_frame_t *out)
{
    uint16_t epoch = 0u;
    uint16_t counter = 0u;
    if (!secoc_tx_next(&g_tx, can_id, &epoch, &counter))
    {
        return false;
    }
    size_t n = secoc_secure(&g_cy, can_id, epoch, counter, pdu, pdu_len,
                            out->data, sizeof out->data);
    if (n == 0u)
    {
        return false;
    }
    out->id = (uint32_t)can_id;
    out->flags = CAN_FLAG_FDF;
    out->len = (uint8_t)n;
    return true;
}

bool secoc_app_build_door_cmd(bool locked, can_raw_frame_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    /* Door PDU is 1 byte (body_decode/MSG_ID_DOOR_CMD): 0 = LOCK, 1 = UNLOCK. */
    uint8_t pdu[1];
    pdu[0] = locked ? (uint8_t)DOOR_LOCK : (uint8_t)DOOR_UNLOCK;
    return build_secured(MSG_ID_DOOR_CMD, pdu, sizeof pdu, out);
}

bool secoc_app_build_light_cmd(uint8_t brightness_pct, can_raw_frame_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    light_cmd_msg_t lc = { .brightness_pct = brightness_pct };
    uint8_t pdu[1];
    size_t plen = pack_light_cmd(&lc, pdu, sizeof pdu);
    if (plen == 0u)
    {
        return false;
    }
    return build_secured(MSG_ID_LIGHT_CMD, pdu, plen, out);
}
