/*
 * can_hal_fake.c — host fake for can_hal_if_t (ADR-0011 D5's "host fake: send()
 * records frames; fake_can_inject_rx() pushes frames as if received", extended
 * with recv() for the M3 poll-based transport). Test harness — exempt from
 * MISRA (see docs/coding-standard.md).
 */
#include "can_hal.h"
#include <string.h>

#define FAKE_CAN_RX_DEPTH   16U
#define FAKE_CAN_TX_DEPTH   32U

static can_raw_frame_t g_rx_q[FAKE_CAN_RX_DEPTH];
static size_t          g_rx_head;
static size_t          g_rx_count;

static can_raw_frame_t g_tx_log[FAKE_CAN_TX_DEPTH];
static size_t          g_tx_count;

void fake_can_reset(void)
{
    memset(g_rx_q, 0, sizeof g_rx_q);
    g_rx_head = 0U;
    g_rx_count = 0U;
    memset(g_tx_log, 0, sizeof g_tx_log);
    g_tx_count = 0U;
}

void fake_can_inject_rx(const can_raw_frame_t *f)
{
    if (g_rx_count < FAKE_CAN_RX_DEPTH)
    {
        size_t tail = (g_rx_head + g_rx_count) % FAKE_CAN_RX_DEPTH;
        g_rx_q[tail] = *f;
        ++g_rx_count;
    }
}

size_t fake_can_sent_count(void) { return g_tx_count; }

const can_raw_frame_t *fake_can_sent_frame(size_t idx)
{
    return (idx < g_tx_count) ? &g_tx_log[idx] : NULL;
}

static int fake_can_init(const can_cfg_t *cfg) { (void)cfg; return 0; }

static int fake_can_send(const can_raw_frame_t *frame)
{
    if (g_tx_count < FAKE_CAN_TX_DEPTH)
    {
        g_tx_log[g_tx_count] = *frame;
        ++g_tx_count;
    }
    return 0;
}

static int fake_can_recv(can_raw_frame_t *out)
{
    if (g_rx_count == 0U) { return -1; }
    *out = g_rx_q[g_rx_head];
    g_rx_head = (g_rx_head + 1U) % FAKE_CAN_RX_DEPTH;
    --g_rx_count;
    return 0;
}

static const can_hal_if_t g_fake_can_hal = {
    .init = fake_can_init,
    .send = fake_can_send,
    .recv = fake_can_recv
};

const can_hal_if_t *fake_can_hal(void) { return &g_fake_can_hal; }
