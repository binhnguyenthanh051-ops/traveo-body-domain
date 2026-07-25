/*
 * can_task.h — Node B (CM7) CAN task entry points (see can_task.c).
 */
#ifndef CAN_TASK_H
#define CAN_TASK_H

#include "FreeRTOS.h"
#include "queue.h"

/* Bring up the CANFD channel (Cy_CANFD_Init + interrupt routing, loopback in
 * Phase A) and create the CAN RX/TX task. Call before vTaskStartScheduler(). */
void can_task_create(void);

/* The RX frame queue the ISR feeds and the task drains (exposed for tests/wiring). */
QueueHandle_t raw_frame_queue(void);

#endif /* CAN_TASK_H */
