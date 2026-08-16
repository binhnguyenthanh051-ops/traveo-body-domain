/*
 * tasks.h — Node A task set wiring (ADR-0010 D5).
 *
 * Target-only. Each task owns its static TCB + stack and a create() function;
 * main() calls the three creators before starting the scheduler. The two RX
 * transport queues are owned here and shared via accessors: the CAN ISR pushes
 * raw frames onto raw_frame_queue(), CAN_CyclicTask decodes and forwards onto
 * app_msg_queue(), App_CyclicTask consumes decoded structs.
 *
 *   Health_CyclicTask  prio 1   plain-periodic (vTaskDelayUntil), no queue
 *   App_CyclicTask     prio 2   event-fed (app_msg_queue) + period
 *   CAN_CyclicTask     prio 3   event-fed (raw_frame_queue) + period; echo
 */
#ifndef TASKS_H
#define TASKS_H

#include "FreeRTOS.h"
#include "queue.h"
#include "can_hal.h"

/* Phase A (=1): on-chip loopback bring-up, SecOC dormant. Phase B (=0, default):
 * real bus with SecOC active. Defined here so can_task and app_task agree (M5). */
#ifndef CAN_LOOPBACK_TEST
#define CAN_LOOPBACK_TEST   0
#endif

/* Task priorities — note these run OPPOSITE to NVIC interrupt priority (D5).
 * LOG sits below everything: draining the log must never delay CAN, the app
 * FSM, or health (ADR-0023 D7). */
#define APP_PRIO_LOG        1
#define APP_PRIO_HEALTH     2
#define APP_PRIO_APP        3
#define APP_PRIO_CAN        4

/* Cycle periods (ms) — the queue-receive timeout for the event-fed tasks. */
#define APP_PERIOD_HEALTH_MS    500U
#define APP_PERIOD_APP_MS        20U
#define APP_PERIOD_CAN_MS        10U

/* Must stay <= LOG_DRAIN_LATENCY_MS (log_types.h): it IS the published bound a
 * BVT absence-assertion waits out before concluding an event never happened. */
#define APP_PERIOD_LOG_MS        20U

/* Task creators (static allocation; assert on failure). */
void health_task_create(void);
void can_task_create(void);
void app_task_create(void);
void log_task_create(void);

/* Shared transport queues (created inside their owning task module). */
QueueHandle_t raw_frame_queue(void);   /* CAN ISR -> CAN_CyclicTask */
QueueHandle_t app_msg_queue(void);     /* CAN_CyclicTask -> App_CyclicTask */

/* Transmit a frame on the single TX buffer (wraps can_task's can_tx). Used by
 * App_CyclicTask to send secured commands (M5). NOTE (ADR-0011 S5): the single
 * TX buffer is shared with any echo TX — back-to-back sends must poll TXBRP; low
 * command rates make this a bring-up concern, not a hot path. */
int can_app_send(const can_raw_frame_t *frame);

#endif /* TASKS_H */
