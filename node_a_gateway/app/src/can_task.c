/*
 * can_task.c — CAN_CyclicTask + the RX ISR/callback (ADR-0010 D5, ADR-0011).
 *
 * Target-only. The Device Configurator generates the CANFD0 channel-1 config
 * (bit timing, message RAM, RX FIFO 0 + accept filter, pins P0.2/P0.3) and its
 * init. This file provides the FreeRTOS glue the configurator does NOT:
 *   - can_rx_callback : invoked by Cy_CANFD_IrqHandler per received frame; packs
 *                       a can_raw_frame_t and pushes it onto raw_frame_queue().
 *                       Set the personality's "Receive callback" to this name.
 *   - can_rx_isr      : the channel ISR — runs Cy_CANFD_IrqHandler then yields.
 *                       Route the CANFD channel-1 interrupt to this (NVIC level 5,
 *                       kernel-aware, ADR-0011 D3).
 *   - can_tx          : the M2 echo — re-transmit a received frame.
 * CAN_CyclicTask receives raw frames, decodes via shared/messages, forwards the
 * decoded struct to App_CyclicTask, and echoes the frame.
 *
 * Bind the two configurator-generated names below (CAN_CHANNEL_CONFIG, CAN_IRQ_SRC).
 */
#include "tasks.h"
#include "task.h"
#include "can_hal.h"
#include "body_msgs.h"
#include "cy_pdl.h"     /* Cy_CANFD_*, Cy_SysInt_* */
#include "cybsp.h"      /* generated canfd_0_chan_1_* (config, IRQ) via cycfg */
#include <string.h>

#define CAN_STACK_WORDS     512U   /* generous for bring-up; trim from high-water (ADR-0010 D5) */
#define RAW_FRAME_QDEPTH     16U

/* CANFD channel for the gateway (ADR-0011). The kit routes CAN0 channel 1 to the
 * onboard transceiver on P0.2 (TX) / P0.3 (RX). Bitrates per ADR-0011 D1 — full
 * CAN FD: 500 kbit/s nominal, 2 Mbit/s data + BRS. The message-RAM partition and
 * the bit-timing register values come from the Device Configurator (seam 2). */
#define CAN_HW_INSTANCE      CANFD0
#define CAN_HW_CHANNEL       1U
#define CAN_TX_PORT          GPIO_PRT0
#define CAN_TX_PIN           2U
#define CAN_RX_PORT          GPIO_PRT0
#define CAN_RX_PIN           3U
#define CAN_NOMINAL_BITRATE  500000U
#define CAN_DATA_BITRATE     2000000U
#define CAN_TX_BUF_IDX       0U     /* the single dedicated TX buffer (ADR-0011 D4) */
#define CAN_NVIC_MUX         NvicMux3_IRQn   /* free CM4 NVIC mux channel for the CAN system int */

/* Bound to the Device Configurator output (cycfg_peripherals): the channel config
 * canfd_0_chan_1_config (its .rxCallback already points at can_rx_callback) and the
 * RX interrupt line canfd_0_chan_1_IRQ_0. CAN_HW_INSTANCE / CAN_HW_CHANNEL above
 * equal the generated canfd_0_chan_1_HW / _CHANNEL_NUM. cybsp_init() assigns the
 * CANFD clock but does NOT init the channel — can_task_create() does. */

/* Phase A bring-up: internal loopback self-test. TX is routed back to RX on-chip,
 * so NO transceiver / bus / CAN tool is needed. The echo is disabled and the task
 * self-transmits a test frame each cycle. Set to 0 for the real bus (Phase B). */
#ifndef CAN_LOOPBACK_TEST
#define CAN_LOOPBACK_TEST   1
#endif

#if CAN_LOOPBACK_TEST
/* Stage counters — read in the debugger to localise where the path stalls:
 *   tx_count climbs, isr stays 0      -> TX issued but no interrupt: loopback not
 *                                        engaged, or frame not received/accepted.
 *   isr climbs, cb stays 0            -> ISR fires but IrqHandler sees no RF0N.
 *   cb climbs, rx_count stays 0       -> callback runs but the queue send fails.
 *   tx_count stays 0 / tx_status != 0 -> Cy_CANFD_UpdateAndTransmitMsgBuffer fails. */
volatile uint32_t g_can_rx_count;   /* task: frames dequeued */
volatile uint32_t g_can_last_id;    /* id of the last dequeued frame */
volatile uint32_t g_can_isr_count;  /* can_rx_isr entries */
volatile uint32_t g_can_cb_count;   /* PDL rx-callback invocations */
volatile uint32_t g_can_tx_count;   /* self-transmit attempts */
volatile int32_t  g_can_tx_status;  /* last can_tx() return (0 = ok) */
volatile int32_t  g_can_cce_status; /* Cy_CANFD_ConfigChangesEnable return (0 = ok) */
volatile uint32_t g_can_cccr;       /* CCCR after loopback: TEST=bit7, MON=bit5 must be 1 */
volatile uint32_t g_can_test_reg;   /* TEST reg after loopback: LBCK=bit4 must be 1 */
#endif

static StaticTask_t  s_tcb;
static StackType_t   s_stack[CAN_STACK_WORDS];

static StaticQueue_t s_raw_q_ctrl;
static uint8_t       s_raw_q_store[RAW_FRAME_QDEPTH * sizeof(can_raw_frame_t)];
static QueueHandle_t s_raw_q;

static cy_stc_canfd_context_t s_canfd_context;
static volatile BaseType_t    s_rx_woken;

QueueHandle_t raw_frame_queue(void) { return s_raw_q; }

/* CAN FD DLC <-> byte-length (DLC 0..8 == bytes; 9..15 -> 12,16,20,24,32,48,64). */
static uint8_t dlc_to_len(uint32_t dlc)
{
    static const uint8_t lut[16] = { 0U,1U,2U,3U,4U,5U,6U,7U,8U,
                                     12U,16U,20U,24U,32U,48U,64U };
    return lut[dlc & 0x0FU];
}

static uint32_t len_to_dlc(uint8_t len)
{
    uint32_t dlc;
    if      (len <= 8U)  { dlc = len; }
    else if (len <= 12U) { dlc = 9U;  }
    else if (len <= 16U) { dlc = 10U; }
    else if (len <= 20U) { dlc = 11U; }
    else if (len <= 24U) { dlc = 12U; }
    else if (len <= 32U) { dlc = 13U; }
    else if (len <= 48U) { dlc = 14U; }
    else                 { dlc = 15U; }
    return dlc;
}

/* PDL RX callback — runs inside Cy_CANFD_IrqHandler for each received message. */
void can_rx_callback(bool rxFIFOMsg, uint8_t bufOrFifoNum, cy_stc_canfd_rx_buffer_t *msg)
{
    (void)rxFIFOMsg;
    (void)bufOrFifoNum;
#if CAN_LOOPBACK_TEST
    ++g_can_cb_count;
#endif

    can_raw_frame_t f;
    f.id    = (uint32_t)msg->r0_f->id;
    f.flags = 0U;
    if (msg->r0_f->xtd == CY_CANFD_XTD_EXTENDED_ID)  { f.flags |= CAN_FLAG_IDE; }
    if (msg->r0_f->rtr == CY_CANFD_RTR_REMOTE_FRAME) { f.flags |= CAN_FLAG_RTR; }
    if (msg->r1_f->fdf == CY_CANFD_FDF_CAN_FD_FRAME) { f.flags |= CAN_FLAG_FDF; }
    if (msg->r1_f->brs)                              { f.flags |= CAN_FLAG_BRS; }
    f.len = dlc_to_len(msg->r1_f->dlc);
    (void)memcpy(f.data, msg->data_area_f, f.len);

    BaseType_t woken = pdFALSE;
    (void)xQueueSendFromISR(s_raw_q, &f, &woken);
    if (woken == pdTRUE)
    {
        s_rx_woken = pdTRUE;   /* the ISR yields after Cy_CANFD_IrqHandler returns */
    }
}

/* Channel ISR — NVIC level 5 (0xA0), kernel-aware (ADR-0011 D3). Route the CANFD
 * channel-1 interrupt here (configurator or Cy_SysInt). */
void can_rx_isr(void)
{
#if CAN_LOOPBACK_TEST
    ++g_can_isr_count;
#endif
    s_rx_woken = pdFALSE;
    Cy_CANFD_IrqHandler(CAN_HW_INSTANCE, CAN_HW_CHANNEL, &s_canfd_context);
    portYIELD_FROM_ISR(s_rx_woken);
}

/* M2 echo: re-transmit a received frame on the single TX buffer. */
static int can_tx(const can_raw_frame_t *f)
{
    cy_stc_canfd_t0_t t0 = { 0 };
    cy_stc_canfd_t1_t t1 = { 0 };
    uint32_t          data[CAN_MAX_DLEN / 4U] = { 0 };
    cy_stc_canfd_tx_buffer_t tx = { .t0_f = &t0, .t1_f = &t1, .data_area_f = data };

    t0.id  = f->id;
    t0.rtr = ((f->flags & CAN_FLAG_RTR) != 0U) ? CY_CANFD_RTR_REMOTE_FRAME : CY_CANFD_RTR_DATA_FRAME;
    t0.xtd = ((f->flags & CAN_FLAG_IDE) != 0U) ? CY_CANFD_XTD_EXTENDED_ID  : CY_CANFD_XTD_STANDARD_ID;
    t1.dlc = len_to_dlc(f->len);
    t1.fdf = ((f->flags & CAN_FLAG_FDF) != 0U) ? CY_CANFD_FDF_CAN_FD_FRAME : CY_CANFD_FDF_STANDARD_FRAME;
    t1.brs = ((f->flags & CAN_FLAG_BRS) != 0U);
    (void)memcpy(data, f->data, f->len);

    return (Cy_CANFD_UpdateAndTransmitMsgBuffer(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                                &tx, CAN_TX_BUF_IDX, &s_canfd_context)
            == CY_CANFD_SUCCESS) ? 0 : -1;
}

static void can_task(void *arg)
{
    (void)arg;
    can_raw_frame_t frame;
    body_msg_t      msg;

    for (;;)
    {
        if (xQueueReceive(s_raw_q, &frame, pdMS_TO_TICKS(APP_PERIOD_CAN_MS)) == pdTRUE)
        {
#if CAN_LOOPBACK_TEST
            ++g_can_rx_count;            /* observe in the debugger: the RX path is alive */
            g_can_last_id = frame.id;
#endif
            if (body_decode(frame.id, frame.data, frame.len, &msg) == 1)
            {
                (void)xQueueSend(app_msg_queue(), &msg, 0);
            }
#if !CAN_LOOPBACK_TEST
            (void)can_tx(&frame);       /* M2 echo: prove the RX->TX loop on one node */
#endif
        }
        else
        {
#if CAN_LOOPBACK_TEST
            /* Loopback self-test: emit a sensor report so RX has traffic (echo is
             * off above to avoid a TX->RX storm). door_ajar toggles, so the decoded
             * struct drives App_CyclicTask -> bodyctl's courtesy-light rule. */
            static uint8_t ajar = 0U;
            can_raw_frame_t t = { .id = MSG_ID_SENSOR_RPT, .flags = CAN_FLAG_FDF, .len = 3U };
            t.data[0] = 0x00U;          /* ambient high byte */
            t.data[1] = 0x64U;          /* ambient ~100      */
            ajar ^= 1U;
            t.data[2] = ajar;           /* door_ajar 0/1     */
            g_can_tx_status = can_tx(&t);
            ++g_can_tx_count;
#endif
        }
    }
}

void can_task_create(void)
{
    s_raw_q = xQueueCreateStatic(RAW_FRAME_QDEPTH, sizeof(can_raw_frame_t),
                                 s_raw_q_store, &s_raw_q_ctrl);
    configASSERT(s_raw_q != NULL);

    /* Bring up CANFD0 channel 1 from the configurator config (rxCallback already
     * = can_rx_callback). cybsp_init() assigned the CANFD clock; init the channel
     * here. If Cy_CANFD_Init errors (or MRAM is inaccessible), call
     * Cy_CANFD_Enable(CAN_HW_INSTANCE, 1UL << CAN_HW_CHANNEL) first (PDL note). */
    cy_en_canfd_status_t st = Cy_CANFD_Init(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                            &canfd_0_chan_1_config, &s_canfd_context);
    configASSERT(st == CY_CANFD_SUCCESS);
    (void)st;

    /* Route the channel RX interrupt to can_rx_isr at NVIC level 5 (kernel-aware,
     * ADR-0011 D3). On TRAVEO T2G the CM4 reaches peripheral interrupts through an
     * 8-channel NVIC mux: intrSrc PACKS the mux channel (high bits) with the system
     * interrupt (canfd_0_chan_1_IRQ_0 = 58), and we enable the MUX CHANNEL — not the
     * bare system interrupt (that earlier mistake mapped 58 -> NvicMux0 silently and
     * enabled a non-existent NVIC line, so no ISR ever fired). Cy_SysInt_Init applies
     * the 3-bit priority shift, so 5 -> raw 0xA0. */
    const cy_stc_sysint_t can_irq =
    {
        .intrSrc      = (cy_sysint_int_src_t)
                        (((uint32_t)CAN_NVIC_MUX << CY_SYSINT_INTRSRC_MUXIRQ_SHIFT)
                         | (uint32_t)canfd_0_chan_1_IRQ_0),
        .intrPriority = 5U,
    };
    (void)Cy_SysInt_Init(&can_irq, &can_rx_isr);
    NVIC_EnableIRQ(CAN_NVIC_MUX);

#if CAN_LOOPBACK_TEST
    /* Phase A: internal loopback (TX -> RX on-chip; no transceiver/bus/tool). The
     * TEST/MON/LBCK bits are protected, so wrap the change in config mode. Capture
     * the result + registers so the debugger shows whether it actually engaged. */
    g_can_cce_status = (int32_t)Cy_CANFD_ConfigChangesEnable(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
    Cy_CANFD_TestModeConfig(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                            CY_CANFD_TEST_MODE_INTERNAL_LOOP_BACK);
    (void)Cy_CANFD_ConfigChangesDisable(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
    g_can_cccr     = CANFD_CCCR(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
    g_can_test_reg = CANFD_TEST(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
#endif

    TaskHandle_t h = xTaskCreateStatic(can_task, "can", CAN_STACK_WORDS,
                                       NULL, APP_PRIO_CAN, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
