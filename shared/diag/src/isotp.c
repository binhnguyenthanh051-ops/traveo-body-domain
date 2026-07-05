/*
 * isotp.c — ISO-TP transport (ADR-0012 layer 1, ADR-0013).
 *
 * Poll-based (ADR-0012 D2): isotp_poll() drains available CAN RX, advances
 * the SF/FF/CF/FC state machine, and sends flow control synchronously when a
 * reception needs a CTS or a pending TX has one. PCI layout: isotp_types.h.
 */
#include "isotp.h"
#include <string.h>

/* The true FD-frame max (64 - 5-byte FF header = 59) -- not 58 as an earlier
 * draft had it "for margin". That one byte of slack was actually harmful:
 * 5 + 58 = 63 is not a valid CAN FD DLC-representable length (0-8, then
 * 12/16/20/24/32/48/64), so the frame silently got padded to 64 on the wire,
 * and a receiver trusting the frame's own reported length (post-padding)
 * would read one extra zero byte as payload (silicon-verified during Seam 2
 * bring-up). Using the exact valid length removes the padding entirely for
 * this frame; handle_cf() below additionally stops trusting frame length at
 * all, since a CF's length is not always a nice round number. */
#define ISOTP_FF_INITIAL_LEN   59U

static const can_hal_if_t *g_can;
static uint32_t g_tx_id;

/* ---- RX reassembly ---- */
static isotp_state_t g_state = ISOTP_IDLE;
static uint8_t  g_rx_buf[ISOTP_MAX_PAYLOAD];
static uint32_t g_rx_total_len;
static uint32_t g_rx_received_len;
static uint8_t  g_rx_next_seq;
static uint32_t g_rx_last_activity_ms;
static bool     g_rx_complete;

/* ---- TX segmentation ---- */
static uint8_t  g_tx_data[ISOTP_MAX_PAYLOAD];
static uint32_t g_tx_total_len;
static uint32_t g_tx_sent_len;
static uint8_t  g_tx_next_seq;
static bool     g_tx_pending;
static uint16_t g_tx_block_remaining;

void isotp_init(const can_hal_if_t *can, uint32_t tx_id)
{
    g_can = can;
    g_tx_id = tx_id;
    g_state = ISOTP_IDLE;
    g_rx_total_len = 0U;
    g_rx_received_len = 0U;
    g_rx_next_seq = 0U;
    g_rx_last_activity_ms = 0U;
    g_rx_complete = false;
    g_tx_total_len = 0U;
    g_tx_sent_len = 0U;
    g_tx_next_seq = 0U;
    g_tx_pending = false;
    g_tx_block_remaining = 0U;
}

static void send_fc(uint8_t status, uint8_t bs, uint8_t stmin)
{
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.id = g_tx_id;
    f.flags = CAN_FLAG_FDF;
    f.data[0] = (uint8_t)(ISOTP_PCI_TYPE_FC | (status & 0x0FU));
    f.data[1] = bs;
    f.data[2] = stmin;
    f.len = 3U;
    (void)g_can->send(&f);
}

static void tx_send_block(void)
{
    while ((g_tx_sent_len < g_tx_total_len) && (g_tx_block_remaining > 0U))
    {
        uint32_t remaining = g_tx_total_len - g_tx_sent_len;
        uint8_t chunk = (uint8_t)((remaining > 63U) ? 63U : remaining);

        can_raw_frame_t f;
        memset(&f, 0, sizeof f);
        f.id = g_tx_id;
        f.flags = CAN_FLAG_FDF;
        f.data[0] = (uint8_t)(ISOTP_PCI_TYPE_CF | (g_tx_next_seq & 0x0FU));
        memcpy(&f.data[1], &g_tx_data[g_tx_sent_len], chunk);
        f.len = (uint8_t)(1U + chunk);
        (void)g_can->send(&f);

        g_tx_sent_len += chunk;
        g_tx_next_seq = (uint8_t)((g_tx_next_seq + 1U) & 0x0FU);
        g_tx_block_remaining--;
    }
    if (g_tx_sent_len >= g_tx_total_len)
    {
        g_tx_pending = false;
    }
}

int isotp_send(const uint8_t *buf, size_t len)
{
    if ((len > ISOTP_MAX_PAYLOAD) || g_tx_pending) { return -1; }

    if (len <= 62U)
    {
        can_raw_frame_t f;
        memset(&f, 0, sizeof f);
        f.id = g_tx_id;
        f.flags = CAN_FLAG_FDF;
        f.data[0] = ISOTP_PCI_TYPE_SF;
        f.data[1] = (uint8_t)len;
        memcpy(&f.data[2], buf, len);
        f.len = (uint8_t)(2U + len);
        (void)g_can->send(&f);
        return 0;
    }

    memcpy(g_tx_data, buf, len);
    g_tx_total_len = (uint32_t)len;

    uint8_t initial = ISOTP_FF_INITIAL_LEN;
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.id = g_tx_id;
    f.flags = CAN_FLAG_FDF;
    f.data[0] = ISOTP_PCI_TYPE_FF;
    f.data[1] = (uint8_t)(g_tx_total_len >> 24);
    f.data[2] = (uint8_t)(g_tx_total_len >> 16);
    f.data[3] = (uint8_t)(g_tx_total_len >> 8);
    f.data[4] = (uint8_t)(g_tx_total_len);
    memcpy(&f.data[5], g_tx_data, initial);
    f.len = (uint8_t)(5U + initial);
    (void)g_can->send(&f);

    g_tx_sent_len = initial;
    g_tx_next_seq = 1U;
    g_tx_pending = true;
    g_tx_block_remaining = 0U;   /* nothing more until a flow-control CTS arrives */
    return 0;
}

static void handle_sf(const can_raw_frame_t *f)
{
    /* len is a byte (0..255, PCI layout) and g_rx_buf is ISOTP_MAX_PAYLOAD --
     * always in range, no overflow check needed here. */
    uint8_t len = f->data[1];
    memcpy(g_rx_buf, &f->data[2], len);
    g_rx_received_len = len;
    g_rx_complete = true;
    g_state = ISOTP_RX_COMPLETE;
}

static void handle_ff(const can_raw_frame_t *f, uint32_t now_ms)
{
    uint32_t total_len = ((uint32_t)f->data[1] << 24) | ((uint32_t)f->data[2] << 16) |
                         ((uint32_t)f->data[3] << 8)  |  (uint32_t)f->data[4];
    if (total_len > ISOTP_MAX_PAYLOAD)
    {
        g_state = ISOTP_ERROR_OVERFLOW;
        return;
    }

    /* ISOTP_FF_INITIAL_LEN, not f->len - 5: a CAN FD frame's own reported
     * length reflects the wire DLC bucket (post-padding, e.g. 63 real bytes
     * rounds up to 64), not necessarily the real payload count. FF's initial
     * chunk is always exactly this fixed size by construction (isotp_send()
     * only reaches the FF path when total_len > 62, so there are always at
     * least this many real bytes available). */
    uint8_t initial_len = ISOTP_FF_INITIAL_LEN;
    memcpy(g_rx_buf, &f->data[5], initial_len);
    g_rx_received_len = initial_len;
    g_rx_total_len = total_len;
    g_rx_next_seq = 1U;
    g_rx_last_activity_ms = now_ms;
    g_state = ISOTP_RX_IN_PROGRESS;

    send_fc(ISOTP_FC_STATUS_CTS, ISOTP_BLOCK_SIZE, (uint8_t)ISOTP_STMIN_MS);
}

static void handle_cf(const can_raw_frame_t *f, uint32_t now_ms)
{
    if (g_state != ISOTP_RX_IN_PROGRESS) { return; }

    uint8_t seq = f->data[0] & 0x0FU;
    if (seq != g_rx_next_seq)
    {
        g_state = ISOTP_ERROR_FLOW;
        return;
    }

    /* Bytes still needed, not f->len - 1: a CF's payload length is often not
     * a valid CAN FD DLC bucket, so the wire frame may be padded longer than
     * the real continuation data -- trusting f->len would copy trailing
     * padding as if it were message content (this exact bug, silicon-
     * verified during Seam 2 bring-up). Deriving the count from the ISO-TP
     * logical state (how much of the declared total is still missing) is
     * immune to that padding regardless of chunk size. */
    uint32_t remaining_needed = g_rx_total_len - g_rx_received_len;
    uint8_t payload_len = (uint8_t)((remaining_needed > 63U) ? 63U : remaining_needed);
    memcpy(&g_rx_buf[g_rx_received_len], &f->data[1], payload_len);
    g_rx_received_len += payload_len;
    g_rx_next_seq = (uint8_t)((g_rx_next_seq + 1U) & 0x0FU);
    g_rx_last_activity_ms = now_ms;

    if (g_rx_received_len >= g_rx_total_len)
    {
        g_rx_complete = true;
        g_state = ISOTP_RX_COMPLETE;
    }
}

static void handle_fc(const can_raw_frame_t *f)
{
    if (!g_tx_pending) { return; }

    uint8_t status = f->data[0] & 0x0FU;
    if (status == ISOTP_FC_STATUS_CTS)
    {
        uint8_t bs = f->data[1];
        g_tx_block_remaining = (bs == 0U) ? 0xFFFFU : bs;
        tx_send_block();
    }
    else if (status == ISOTP_FC_STATUS_OVERFLOW)
    {
        g_tx_pending = false;
        g_state = ISOTP_ERROR_OVERFLOW;
    }
    else
    {
        /* WAIT: nothing to do until another FC arrives. */
    }
}

void isotp_poll(uint32_t now_ms)
{
    can_raw_frame_t f;
    while ((g_can != NULL) && (g_can->recv != NULL) && (g_can->recv(&f) == 0))
    {
        uint8_t type = f.data[0] & 0xF0U;
        switch (type)
        {
            case ISOTP_PCI_TYPE_SF: handle_sf(&f); break;
            case ISOTP_PCI_TYPE_FF: handle_ff(&f, now_ms); break;
            case ISOTP_PCI_TYPE_CF: handle_cf(&f, now_ms); break;
            case ISOTP_PCI_TYPE_FC: handle_fc(&f); break;
            default: break;
        }
    }

    if ((g_state == ISOTP_RX_IN_PROGRESS) &&
        ((now_ms - g_rx_last_activity_ms) >= ISOTP_N_CR_MS))
    {
        g_state = ISOTP_ERROR_TIMEOUT;
    }
}

bool isotp_take_received(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    if (!g_rx_complete) { return false; }
    if (g_rx_received_len > buf_cap) { return false; }
    memcpy(buf, g_rx_buf, g_rx_received_len);
    *out_len = g_rx_received_len;
    g_rx_complete = false;
    g_state = ISOTP_IDLE;
    return true;
}

isotp_state_t isotp_current_state(void)
{
    return g_state;
}
