/*
 * can_task.c — CAN_CyclicTask + the RX ISR (ADR-0010 D5, ADR-0011 D3).
 *
 * Target-only. Owns raw_frame_queue(): the CAN RX ISR drains the hardware FIFO
 * and pushes raw frames; the task receives them (event-fed, with a period
 * timeout), decodes via shared/messages, forwards decoded body_msg_t onto
 * app_msg_queue(), and (M2 echo) re-transmits the received frame.
 *
 * Stub: the CANFD PDL init / FIFO read / TX are marked TODO until bring-up. The
 * ISR shape (xQueueSendFromISR + portYIELD_FROM_ISR at NVIC level 5) is final.
 */
#include "tasks.h"
#include "task.h"
#include "can_hal.h"
#include "body_msgs.h"

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

static StaticTask_t  s_tcb;
static StackType_t   s_stack[CAN_STACK_WORDS];

static StaticQueue_t s_raw_q_ctrl;
static uint8_t       s_raw_q_store[RAW_FRAME_QDEPTH * sizeof(can_raw_frame_t)];
static QueueHandle_t s_raw_q;

QueueHandle_t raw_frame_queue(void) { return s_raw_q; }

/* CAN RX interrupt — runs at NVIC level 5 (0xA0), kernel-aware (ADR-0011 D3).
 * Registered against the CANFD RF0N source during init. */
void can_rx_isr(void)
{
    BaseType_t woken = pdFALSE;
    can_raw_frame_t frame;

    /* TODO(bring-up): drain RX FIFO 0 until empty into `frame`(s); for each:
     *   xQueueSendFromISR(s_raw_q, &frame, &woken);
     * then clear the RF0N flag (write-1-clear). */
    (void)frame;

    portYIELD_FROM_ISR(woken);
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
            if (body_decode(frame.id, frame.data, frame.len, &msg) == 1)
            {
                (void)xQueueSend(app_msg_queue(), &msg, 0);
            }
            /* M2 echo: prove the RX->TX loop on a single node. */
            /* TODO(bring-up): can_hal.send(&frame); */
        }
        /* Period elapsed with no frame: cyclic housekeeping slot (none yet). */
    }
}

void can_task_create(void)
{
    s_raw_q = xQueueCreateStatic(RAW_FRAME_QDEPTH, sizeof(can_raw_frame_t),
                                 s_raw_q_store, &s_raw_q_ctrl);
    configASSERT(s_raw_q != NULL);

    /* TODO(bring-up): can_hal.init(&cfg) with full CAN FD (ADR-0011 D1), then
     * NVIC_SetPriority(<canfd irq>, 5) and NVIC_EnableIRQ(...). */

    TaskHandle_t h = xTaskCreateStatic(can_task, "can", CAN_STACK_WORDS,
                                       NULL, APP_PRIO_CAN, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
