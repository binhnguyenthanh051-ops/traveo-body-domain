/*
 * can_task.c — Node B (CM7) CANFD bring-up: RX ISR/callback + TX + loopback test.
 *
 * Target-only. Adapted from Node A's proven app/src/can_task.c (ADR-0010 D5,
 * ADR-0011) for the CM7 core on CYT4BF8CDS (cat1c). The Device Configurator
 * generates the CANFD channel config (bit timing, message RAM, RX FIFO 0 +
 * accept filter, pins) and its init; this file provides the FreeRTOS glue.
 *
 * BRING-UP ORDER (mirrors Node A M2):
 *   Phase A (CAN_LOOPBACK_TEST=1): internal loopback, no transceiver/bus/tool —
 *     proves Init + interrupt routing + RX callback + TX path on CM7.
 *   Phase B (=0): real bus (VN1610 / Node A) — echo, then SecOC later.
 *
 * UNVALIDATED: the CAT1C CM7 interrupt idiom (below) and the configurator
 * bindings (marked TODO) are bench checks. Structure is Node A's proven one.
 *
 * === Bind these from your Device Configurator output (cycfg_peripherals.h) ===
 *   CAN_HW_INSTANCE / CAN_HW_CHANNEL  = generated <name>_HW / _CHANNEL_NUM
 *   CAN_CHANNEL_CONFIG                = generated <name>_config (rxCallback=can_rx_callback)
 *   CAN_SYS_IRQ_SRC                   = the CANFD channel system interrupt (cy_en_intr_t,
 *                                       device header, e.g. canfd_0_interrupts0_0_IRQn)
 *   CAN_CPU_IRQ                       = a FREE CM7 CPU IRQ line to route it onto (IRQn_Type)
 */
#include "FreeRTOS.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "can_task.h"
#include "can_hal.h"
#include "cy_pdl.h"     /* Cy_CANFD_*, Cy_SysInt_* */
#include "cybsp.h"      /* generated CANFD config + IRQ names via cycfg */
#include <string.h>

/* ---- Configurator bindings (from cycfg_peripherals.h: CANFD0 CH2, P12[0]/P12[1]) --- */
#define CAN_HW_INSTANCE      CANFD_0_2_HW           /* = CANFD0 */
#define CAN_HW_CHANNEL       CANFD_0_2_CHANNEL_NUM  /* = 2U (CANFD0_CH2) */
#define CAN_CHANNEL_CONFIG   CANFD_0_2_config       /* generated channel config (rxCallback=can_rx_callback) */
#define CAN_SYS_IRQ_SRC      CANFD_0_2_IRQ_0        /* = canfd_0_interrupts0_2_IRQn (RX FIFO0, line 0) */
/* CM7 CPU IRQ line CAN_SYS_IRQ_SRC is routed onto. CAT1C CM7 uses the same
 * NvicMux0..7 user lines as Node A's CM4, BUT NvicMux0/1 are reserved by ROM, so
 * pick 2..7 and make sure nothing else in this image claims it. If routing is
 * wrong, loopback shows g_can_tx_count climbing while g_can_isr_count stays 0. */
#define CAN_CPU_IRQ          NvicMux3_IRQn   /* free CM7 user line (verify unused) */
#define CAN_IRQ_PRIORITY     5U              /* kernel-aware: >= configMAX_SYSCALL_INTERRUPT_PRIORITY */
#define CAN_TX_BUF_IDX       0U

#define CAN_STACK_WORDS      192U
#define RAW_FRAME_QDEPTH     16U
#define CAN_TASK_PRIO        2U
#define CAN_POLL_MS          20U

#ifndef CAN_LOOPBACK_TEST
#define CAN_LOOPBACK_TEST    1               /* Phase A: on-chip loopback (proven). Phase B (=0): real bus + echo.
                                              * Phase B deferred: needs VN1610 + 120R-terminated bus. */
#endif

/* Stage counters (read in the debugger to localise a stall — same as Node A):
 *   tx climbs, isr 0     -> TX issued, no interrupt: loopback/routing not engaged.
 *   isr climbs, cb 0     -> ISR fires but IrqHandler sees no RF0N.
 *   cb climbs, rx 0      -> callback runs but queue send fails.
 *   tx 0 / tx_status !=0 -> Cy_CANFD_UpdateAndTransmitMsgBuffer fails. */
volatile uint32_t g_can_rx_count;
volatile uint32_t g_can_last_id;
volatile uint32_t g_can_isr_count;
volatile uint32_t g_can_cb_count;
volatile uint32_t g_can_tx_count;
volatile int32_t  g_can_tx_status;

static StaticTask_t  s_tcb;
static StackType_t   s_stack[CAN_STACK_WORDS];
static StaticQueue_t s_raw_q_ctrl;
static uint8_t       s_raw_q_store[RAW_FRAME_QDEPTH * sizeof(can_raw_frame_t)];
static QueueHandle_t s_raw_q;

static cy_stc_canfd_context_t s_canfd_context;
static volatile BaseType_t    s_rx_woken;

QueueHandle_t raw_frame_queue(void) { return s_raw_q; }

/* CAN FD DLC <-> byte length (0..8 == bytes; 9..15 -> 12,16,20,24,32,48,64). */
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

/* PDL RX callback — runs inside Cy_CANFD_IrqHandler per received message. Set the
 * personality's "Receive callback" to this name. */
void can_rx_callback(bool rxFIFOMsg, uint8_t bufOrFifoNum, cy_stc_canfd_rx_buffer_t *msg)
{
    (void)rxFIFOMsg;
    (void)bufOrFifoNum;
    ++g_can_cb_count;
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
    if (woken == pdTRUE) { s_rx_woken = pdTRUE; }
}

/* Channel ISR — route the CANFD channel interrupt here (see can_task_create). */
void can_rx_isr(void)
{
    ++g_can_isr_count;
    s_rx_woken = pdFALSE;
    Cy_CANFD_IrqHandler(CAN_HW_INSTANCE, CAN_HW_CHANNEL, &s_canfd_context);
    portYIELD_FROM_ISR(s_rx_woken);
}

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

    /* NOTE (ADR-0011 S5): for back-to-back TX, poll TXBRP clear before reusing
     * the buffer. The loopback self-test sends one frame per cycle, so it does
     * not hit that; the echo/actuator path will need the wait. */
    return (Cy_CANFD_UpdateAndTransmitMsgBuffer(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                                &tx, CAN_TX_BUF_IDX, &s_canfd_context)
            == CY_CANFD_SUCCESS) ? 0 : -1;
}

static void can_task(void *arg)
{
    (void)arg;
    can_raw_frame_t frame;

    for (;;)
    {
        if (xQueueReceive(s_raw_q, &frame, pdMS_TO_TICKS(CAN_POLL_MS)) == pdTRUE)
        {
            ++g_can_rx_count;            /* RX path alive (both phases) */
            g_can_last_id = frame.id;
#if !CAN_LOOPBACK_TEST
            (void)can_tx(&frame);       /* Phase B echo: re-transmit to prove RX+TX on the real bus */
#endif
            /* Next seam: body_decode(frame.id, frame.data, frame.len, &msg) -> actuator FSM. */
        }
        else
        {
#if CAN_LOOPBACK_TEST
            /* Self-transmit a test frame so the loopback RX path has traffic. */
            can_raw_frame_t t = { .id = 0x123U, .flags = CAN_FLAG_FDF, .len = 4U };
            t.data[0] = 0xDEU; t.data[1] = 0xADU; t.data[2] = 0xBEU; t.data[3] = 0xEFU;
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

    cy_en_canfd_status_t st = Cy_CANFD_Init(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                            &CAN_CHANNEL_CONFIG, &s_canfd_context);
    configASSERT(st == CY_CANFD_SUCCESS);
    (void)st;

    /* --- Interrupt routing (same idiom as Node A, portable via the shift macro) ---
     * cy_stc_sysint_t.intrSrc packs the NvicMux CPU line in the high bits and the
     * system interrupt source in the low bits. CY_SYSINT_INTRSRC_MUXIRQ_SHIFT is
     * 12 on CAT1A (Node A/CM4) and 16 on CAT1C (here/CM7), so this code is
     * platform-portable; then enable the NvicMux line. (NvicMux0/1 = ROM on CAT1C.) */
    const cy_stc_sysint_t can_irq =
    {
        .intrSrc      = (cy_sysint_int_src_t)
                        (((uint32_t)CAN_CPU_IRQ << CY_SYSINT_INTRSRC_MUXIRQ_SHIFT)
                         | (uint32_t)CAN_SYS_IRQ_SRC),
        .intrPriority = CAN_IRQ_PRIORITY,
    };
    (void)Cy_SysInt_Init(&can_irq, &can_rx_isr);
    NVIC_EnableIRQ(CAN_CPU_IRQ);

#if CAN_LOOPBACK_TEST
    /* Phase A: internal loopback (TX -> RX on-chip; no transceiver/bus/tool). The
     * TEST/MON/LBCK bits are protected — wrap in config mode. */
    (void)Cy_CANFD_ConfigChangesEnable(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
    Cy_CANFD_TestModeConfig(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                            CY_CANFD_TEST_MODE_INTERNAL_LOOP_BACK);
    (void)Cy_CANFD_ConfigChangesDisable(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
#endif

    TaskHandle_t h = xTaskCreateStatic(can_task, "can", CAN_STACK_WORDS,
                                       NULL, CAN_TASK_PRIO, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
