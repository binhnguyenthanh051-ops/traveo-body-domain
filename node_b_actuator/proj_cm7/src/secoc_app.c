/*
 * secoc_app.c — Node B SecOC application glue (M5 seam 3, ADR-0021). See header.
 *
 * @impl ADR-0021 D7 : SecOC oracle bound to the M0+ crypto_mac client
 * @impl REQ-SECOC-001 : commands verified (MAC+freshness) before actuation; else dropped
 * @impl REQ-SECOC-008 : boot FRESHNESS_SYNC carries the receiver floor
 * @impl REQ-SECOC-013 : telemetry secured on the identical path
 */
#include "secoc_app.h"
#include "secoc.h"            /* secoc_secure/verify, secoc_crypto_t, secoc_mac_if_t */
#include "secoc_freshness.h"  /* tx/rx contexts + accept rule */
#include "secoc_store.h"      /* ADR-0024 work-flash store */
#include "wflash_port.h"      /* its target HAL instance */
#include "body_msgs.h"         /* body_msg_pdu_len, MSG_ID_* */
#include "secoc_rx.h"         /* the composed RX verdict + SecOC events (ADR-0023 D11) */
#include "secoc_key_id.h"     /* SECOC_MAC_KEY_ID (public selector, no secret) */
#include "crypto_service.h"   /* crypto_mac (M0+ offload client) */
#include "crypto_types.h"     /* CRYPTO_CMAC_TAG_LEN */

secoc_rx_t g_secoc_rx;
volatile uint32_t g_secoc_drop_decode;

/* --- crypto oracle: bind the SecOC MAC port directly to the M0+ client. The
 * signatures match exactly (bool(uint32_t,const uint8_t*,size_t,uint8_t[16])),
 * so no shim is needed. --- */
static const secoc_mac_if_t g_oracle = { crypto_mac };
static const secoc_crypto_t g_cy = { &g_oracle, SECOC_MAC_KEY_ID };

/* --- freshness store: two-slot record in work flash (ADR-0024), behind the
 * ADR-0021 D8 port. Persistent: the sender epoch and the receiver floor survive
 * a power cycle (REQ-SECOC-006/007/008). Slots are 2 KB apart in the small-
 * sector region, so they're separate sectors whether a small sector is 128 B
 * (Body Entry TRM) or larger (ADR-0024 V1). --- */
enum { SECOC_DOM_TX_SELF = 0u, SECOC_DOM_RX_A = 1u };

#define SECOC_STORE_SLOT0   (CY_WFLASH_SM_SBM_BASE)
#define SECOC_STORE_SLOT1   (CY_WFLASH_SM_SBM_BASE + 0x800u)
#define SECOC_WF_SECTOR     128u    /* small sector; unverified Body High value, see ADR-0024 V1 */

/* DMA- and CM0+-visible scratch for the port. The CM7 has a D-cache
 * and neither the M-DMA nor the CM0+ sees it, so this lives in
 * .cy_sharedmem -> ram_noncache (ADR-0018 D6). */
static wflash_port_shared_t s_wf_shared CY_SECTION_SHAREDMEM CY_ALIGN(32);

static const wflash_port_cfg_t g_wf_cfg = {
    .dmac        = DMAC,
    .channel     = 0u,                      /* M-DMA ch 0: unused elsewhere */
    .sw_trigger  = (uint32_t)TRIG_OUT_MUX_3_MDMA_TR_IN0,
    .sector_size = SECOC_WF_SECTOR,
    .shared      = &s_wf_shared,
};

/* Mount result, for the debugger: 0 OK, 1 FACTORY, 2 BROKEN (no SecOC). */
volatile uint32_t g_secoc_store_status = 0xFFFFFFFFu;

static const secoc_freshness_store_if_t *const g_store = &g_secoc_store;

/* --- freshness contexts ---
 * Node B SENDS on 0x200 (telemetry) + 0x2F0 (sync): one sender context, one
 * shared epoch (SECOC_DOM_TX_SELF). Node B RECEIVES commands 0x120/0x121 from A:
 * one receiver context tracking A's sender epoch (SECOC_DOM_RX_A). */
static secoc_tx_ctx_t g_tx;
static secoc_rx_ctx_t g_rx;

static const uint16_t g_tx_ids[] = { MSG_ID_SENSOR_RPT, MSG_ID_FRESHNESS_SYNC };
static const uint16_t g_rx_ids[] = { MSG_ID_DOOR_CMD, MSG_ID_LIGHT_CMD };

void secoc_app_init(void)
{
    const secoc_store_cfg_t sc = {
        .flash     = wflash_port_init(&g_wf_cfg),
        .slot_addr = { SECOC_STORE_SLOT0, SECOC_STORE_SLOT1 },
    };
    secoc_store_status_t st = secoc_store_mount(&sc);
    g_secoc_store_status = (uint32_t)st;

    /* BROKEN fails closed (ADR-0024 D3): no sender boot, so nothing goes out
     * authenticated; no receiver boot, so every frame is dropped as an unknown
     * ID. Booting the receiver cold here would accept every old epoch. */
    if (st != SECOC_STORE_BROKEN)
    {
        (void)secoc_tx_boot(&g_tx, g_store, (uint8_t)SECOC_DOM_TX_SELF,
                            g_tx_ids, sizeof g_tx_ids / sizeof g_tx_ids[0]);
        (void)secoc_rx_boot(&g_rx, g_store, (uint8_t)SECOC_DOM_RX_A,
                            g_rx_ids, sizeof g_rx_ids / sizeof g_rx_ids[0]);
    }
    secoc_rx_init(&g_secoc_rx, &g_cy, &g_rx);
    /* Per-ID PDU lengths, so a CAN FD-padded secured frame verifies (W40 bench). */
    secoc_rx_set_pdu_len_of(&g_secoc_rx, body_msg_pdu_len);
}

bool secoc_app_verify_and_decode(const can_raw_frame_t *f, body_msg_t *out)
{
    if ((f == NULL) || (out == NULL))
    {
        return false;
    }

    uint8_t pdu[CAN_MAX_DLEN];
    size_t  pdu_len = 0u;

    /* MAC then freshness, counted by reason and reported as a contract event —
     * all inside the seam (ADR-0023 D11). What used to be twenty lines here, in
     * two nodes, with the counters and the verdict drifting apart by hand. */
    if (secoc_rx_process(&g_secoc_rx, (uint16_t)f->id, f->data, f->len,
                         pdu, sizeof pdu, &pdu_len) != SECOC_RX_ACCEPT)
    {
        return false;                   /* dropped before decode (ADR-0021 D9) */
    }

    if (body_decode(f->id, pdu, pdu_len, out) != 1)
    {
        ++g_secoc_drop_decode;
        return false;
    }
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
    out->flags = CAN_FLAG_FDF;                 /* CAN FD: payload+trailer in one frame */
    out->len = (uint8_t)n;
    return true;
}

bool secoc_app_build_telemetry(const sensor_report_msg_t *rpt, can_raw_frame_t *out)
{
    if ((rpt == NULL) || (out == NULL))
    {
        return false;
    }
    uint8_t pdu[8];
    size_t plen = pack_sensor_report(rpt, pdu, sizeof pdu);
    if (plen == 0u)
    {
        return false;
    }
    return build_secured(MSG_ID_SENSOR_RPT, pdu, plen, out);
}

bool secoc_app_build_boot_sync(can_raw_frame_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    /* PDU = this receiver's epoch floor for the gateway (u16 LE). The gateway
     * verifies the 0x2F0 frame and adopts max(current, floor) (D5). */
    uint16_t floor = g_rx.floor;
    uint8_t pdu[2];
    pdu[0] = (uint8_t)(floor & 0xFFu);
    pdu[1] = (uint8_t)((floor >> 8) & 0xFFu);
    return build_secured(MSG_ID_FRESHNESS_SYNC, pdu, sizeof pdu, out);
}
