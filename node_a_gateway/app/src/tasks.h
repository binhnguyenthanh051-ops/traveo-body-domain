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

/* Task priorities — note these run OPPOSITE to NVIC interrupt priority (D5). */
#define APP_PRIO_HEALTH     1
#define APP_PRIO_APP        2
#define APP_PRIO_CAN        3

/* Cycle periods (ms) — the queue-receive timeout for the event-fed tasks. */
#define APP_PERIOD_HEALTH_MS    500U
#define APP_PERIOD_APP_MS        20U
#define APP_PERIOD_CAN_MS        10U

/* Task creators (static allocation; assert on failure). */
void health_task_create(void);
void can_task_create(void);
void app_task_create(void);

/* Shared transport queues (created inside their owning task module). */
QueueHandle_t raw_frame_queue(void);   /* CAN ISR -> CAN_CyclicTask */
QueueHandle_t app_msg_queue(void);     /* CAN_CyclicTask -> App_CyclicTask */

#endif /* TASKS_H */
