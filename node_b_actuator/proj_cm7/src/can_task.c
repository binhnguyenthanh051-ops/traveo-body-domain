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
#include "secoc_app.h"      /* SecOC verify/secure/sync (M5 seam 3) */
#include "actuator_fsm.h"   /* actuate only on VALID commands */
#include "body_msgs.h"      /* MSG_ID_*, body_msg_t */
#include "port_crypto.h"    /* secoc_crypto_bringup_report (bench Stage 3) */
#include "tb_log.h"         /* log_evt */
#include "log_events.h"     /* LOG_EVT_DBG_U32 */
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

/* 384: crypto_mac() alone needs ~540 B (2 x crypto_msg_t + 2 wire buffers) on the
 * caller's stack; 192 overflowed on the first MAC (W40 bench, Stage 3). ADR-0010 D5. */
#define CAN_STACK_WORDS      384U
#define RAW_FRAME_QDEPTH     16U
#define CAN_TASK_PRIO        2U
#define CAN_POLL_MS          20U

#ifndef CAN_LOOPBACK_TEST
#define CAN_LOOPBACK_TEST    0               /* Phase B (default since W40 Stage 5): real bus + echo.
                                              * Phase A on-chip loopback: build with CAN_LOOPBACK_TEST=1. */
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

#if !CAN_LOOPBACK_TEST
/* Bench Stage 5 diagnosis: the controller's own view of the bus, once a second,
 * as LOG_EVT_DBG_U32 (no debugger needed):
 *   tag 0x3510 PSR  bits[2:0] LEC = last error: 0 none, 1 stuff, 2 form,
 *                   3 ACK (we sent, nobody acked), 4 bit1, 5 bit0, 7 no change;
 *                   bit 5 EP = error passive, bit 7 BO = bus off
 *   tag 0x3511 ECR  bits[7:0] TEC (we transmit and fail), bits[14:8] REC
 *   tag 0x3512 CCCR bit 0 INIT (stuck in config), bit 5 MON (bus monitoring),
 *                   bit 7 TEST (test/loopback mode still on)
 * Reading PSR resets LEC to 7, so each record covers the last second. */
static void can_report_status(void)
{
    static TickType_t s_last;
    const TickType_t now = xTaskGetTickCount();
    if ((now - s_last) < pdMS_TO_TICKS(1000U))
    {
        return;
    }
    s_last = now;

    volatile CANFD_CH_M_TTCAN_Type const *tt = &CAN_HW_INSTANCE->CH[CAN_HW_CHANNEL].M_TTCAN;
    log_evt(LOG_EVT_DBG_U32, tt->PSR,  0x3510U);
    log_evt(LOG_EVT_DBG_U32, tt->ECR,  0x3511U);
    log_evt(LOG_EVT_DBG_U32, tt->CCCR, 0x3512U);
}
#endif

static void can_task(void *arg)
{
    (void)arg;
    can_raw_frame_t frame;

#if !CAN_LOOPBACK_TEST
    bool sync_sent = false;
#endif

#if defined(SECOC_CRYPTO_BRINGUP) && (SECOC_CRYPTO_BRINGUP != 0)
    /* Bench Stage 3.3/3.4: one CM7 -> CM0+ MAC round trip + cache/MPU state,
     * reported as LOG_EVT_DBG_U32 (tags 0x33xx). Build with SECOC_CRYPTO_BRINGUP=1. */
    secoc_crypto_bringup_report();
#endif

    for (;;)
    {
#if !CAN_LOOPBACK_TEST
        can_report_status();
        /* Phase B: on the real bus, announce our freshness floor once at startup
         * so the gateway raises its command epoch (D5 resync). Task context — the
         * MAC round-trip uses the RTOS-tick clock. */
        if (!sync_sent)
        {
            can_raw_frame_t sync;
            if (secoc_app_build_boot_sync(&sync) && (can_tx(&sync) == 0))
            {
                sync_sent = true;
            }
        }
#endif
        if (xQueueReceive(s_raw_q, &frame, pdMS_TO_TICKS(CAN_POLL_MS)) == pdTRUE)
        {
            ++g_can_rx_count;            /* RX path alive (both phases) */
            g_can_last_id = frame.id;

            if ((frame.id == MSG_ID_DOOR_CMD) || (frame.id == MSG_ID_LIGHT_CMD))
            {
                /* Authenticated command: actuate ONLY on VALID MAC + fresh
                 * freshness; a bad frame is dropped + counted inside SecOC and
                 * the FSM holds its last safe state (ADR-0021 D9). */
                body_msg_t msg;
                if (secoc_app_verify_and_decode(&frame, &msg))
                {
                    actuator_fsm_apply(&msg);
                }
            }
#if !CAN_LOOPBACK_TEST
            else
            {
                (void)can_tx(&frame);   /* Phase B: echo non-command IDs (bring-up aid) */
            }
#endif
        }
        else
        {
#if CAN_LOOPBACK_TEST
            /* Phase A: self-transmit a test frame so the loopback RX path has
             * traffic. 0x123 is not a command ID, so the SecOC RX path above
             * stays dormant — the proven loopback bring-up is unchanged. */
            can_raw_frame_t t = { .id = 0x123U, .flags = CAN_FLAG_FDF, .len = 4U };
            t.data[0] = 0xDEU; t.data[1] = 0xADU; t.data[2] = 0xBEU; t.data[3] = 0xEFU;
            g_can_tx_status = can_tx(&t);
            ++g_can_tx_count;
#else
            /* Phase B: periodic authenticated telemetry (secured 0x200). The
             * ambient value would come from an ADC read; door_ajar reflects the
             * FSM state. */
            sensor_report_msg_t rpt = { .ambient_raw = 0u,
                                        .door_ajar = actuator_door_locked() ? 0u : 1u };
            can_raw_frame_t tf;
            if (secoc_app_build_telemetry(&rpt, &tf))
            {
                (void)can_tx(&tf);
            }
#endif
        }
    }
}

/* The Node B BSP leaves P12_0/P12_1 as analog GPIO (HSIOM_SEL_GPIO) — Phase A's
 * internal loopback never needed the pins, so nothing noticed. Route them to
 * CANFD0 CH2 here, in code, like port_log.c does for the UART, so the real bus
 * does not depend on the regenerable BSP. Mux values from the CYT4BF8CDS GPIO
 * header (canfd[0].ttcan_tx[2] / ttcan_rx[2]). */
static void can_pins_init(void)
{
    Cy_GPIO_SetHSIOM(CYBSP_CAN_RX_PORT, CYBSP_CAN_RX_PIN, P12_1_CANFD0_TTCAN_RX2);
    Cy_GPIO_SetDrivemode(CYBSP_CAN_RX_PORT, CYBSP_CAN_RX_PIN, CY_GPIO_DM_HIGHZ);

    Cy_GPIO_Set(CYBSP_CAN_TX_PORT, CYBSP_CAN_TX_PIN);   /* recessive before the mux switches */
    Cy_GPIO_SetHSIOM(CYBSP_CAN_TX_PORT, CYBSP_CAN_TX_PIN, P12_0_CANFD0_TTCAN_TX2);
    Cy_GPIO_SetDrivemode(CYBSP_CAN_TX_PORT, CYBSP_CAN_TX_PIN, CY_GPIO_DM_STRONG_IN_OFF);
}

/* Nominal bit timing, overriding the configurator's. Node B's design.modus set
 * the nominal phase to 5 x (1+9+4) = 70 CAN clocks per bit = 571 kbit/s at the
 * 40 MHz CAN clock (100 MHz peri / 2.5), while Node A runs 5 x (1+11+4) = 80 =
 * 500 kbit/s. Internal loopback agrees with any bitrate, so Phase A never saw it;
 * on the real bus nobody ACKs Node B. Same values as Node A (ADR-0011: 500 kbit/s,
 * sample point 12/16 = 75%). The data phase (1 x 20 = 2 Mbit/s) already matched.
 * TODO: fix CANFD0 CH2 in design.modus too, then drop this override. */
static const cy_stc_canfd_bitrate_t s_nominal_500k = {
    .prescaler      = 5U - 1U,
    .timeSegment1   = 11U - 1U,
    .timeSegment2   = 4U - 1U,
    .syncJumpWidth  = 4U - 1U,
};

void can_task_create(void)
{
    s_raw_q = xQueueCreateStatic(RAW_FRAME_QDEPTH, sizeof(can_raw_frame_t),
                                 s_raw_q_store, &s_raw_q_ctrl);
    configASSERT(s_raw_q != NULL);

    /* SecOC (seam 3): boot the freshness contexts + safe-state the actuator. No
     * crypto here (the M0+ oracle is only touched from the task), so this is safe
     * before the scheduler. secoc_crypto_port_init() (main.c) must run first. */
    actuator_fsm_init();
    secoc_app_init();

    can_pins_init();

    /* Bring-up evidence for the bit timing above: the CAN clock actually
     * delivered to CANFD0 CH2 (expected 40000000). */
    log_evt(LOG_EVT_DBG_U32,
            Cy_SysClk_PeriPclkGetFrequency(PCLK_CANFD0_CLOCK_CAN2, CY_SYSCLK_DIV_24_5_BIT, 0U),
            0x3500U);

    static cy_stc_canfd_config_t s_can_cfg;   /* static: the driver may keep the pointer */
    s_can_cfg = CAN_CHANNEL_CONFIG;
    s_can_cfg.bitrate = &s_nominal_500k;

    cy_en_canfd_status_t st = Cy_CANFD_Init(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                            &s_can_cfg, &s_canfd_context);
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
