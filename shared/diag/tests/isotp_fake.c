/*
 * isotp_fake.c — host fake for isotp.h, used by uds_session tests so they
 * exercise session/dispatch logic in isolation from the transport's own
 * (separately tested, currently stubbed) implementation. Test harness —
 * exempt from MISRA.
 */
#include "isotp.h"
#include <string.h>

static uint8_t g_rx_buf[ISOTP_MAX_PAYLOAD];
static size_t  g_rx_len;
static bool    g_rx_pending;

static uint8_t g_tx_buf[ISOTP_MAX_PAYLOAD];
static size_t  g_tx_len;
static size_t  g_tx_count;

void fake_isotp_reset(void)
{
    memset(g_rx_buf, 0, sizeof g_rx_buf);
    g_rx_len = 0U;
    g_rx_pending = false;
    memset(g_tx_buf, 0, sizeof g_tx_buf);
    g_tx_len = 0U;
    g_tx_count = 0U;
}

void fake_isotp_inject_received(const uint8_t *buf, size_t len)
{
    memcpy(g_rx_buf, buf, len);
    g_rx_len = len;
    g_rx_pending = true;
}

const uint8_t *fake_isotp_last_sent(size_t *out_len)
{
    *out_len = g_tx_len;
    return g_tx_buf;
}

size_t fake_isotp_send_count(void) { return g_tx_count; }

/* ---- isotp.h implementation ---- */
void isotp_init(const can_hal_if_t *can, uint32_t tx_id) { (void)can; (void)tx_id; }
void isotp_poll(uint32_t now_ms) { (void)now_ms; }

int isotp_send(const uint8_t *buf, size_t len)
{
    if (len > sizeof g_tx_buf) { return -1; }
    memcpy(g_tx_buf, buf, len);
    g_tx_len = len;
    ++g_tx_count;
    return 0;
}

bool isotp_take_received(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    if (!g_rx_pending) { return false; }
    if (g_rx_len > buf_cap) { return false; }
    memcpy(buf, g_rx_buf, g_rx_len);
    *out_len = g_rx_len;
    g_rx_pending = false;
    return true;
}

isotp_state_t isotp_current_state(void) { return ISOTP_IDLE; }
