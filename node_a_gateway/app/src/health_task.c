/*
 * health_task.c — Health_CyclicTask (ADR-0010 D5).
 *
 * Target-only. Plain-periodic (no input queue): toggle the heartbeat LED and
 * sample task stack high-water marks each period. The blinking LED is itself a
 * liveness signal — if a higher-priority task hard-loops, this starves and the
 * LED freezes. M6 adds watchdog servicing + per-task check-ins here.
 *
 * Stub: the LED GPIO and high-water reporting are marked TODO until bring-up.
 */
#include "tasks.h"
#include "task.h"
#include "cybsp.h"

#define HEALTH_STACK_WORDS   160U

static StaticTask_t s_tcb;
static StackType_t  s_stack[HEALTH_STACK_WORDS];

static void health_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;)
    {
        /* TODO(bring-up): toggle the heartbeat LED (cyhal_gpio_toggle). */
        /* TODO(bring-up): sample uxTaskGetStackHighWaterMark for each task and
         * record the minima for the stack-trim pass. */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(APP_PERIOD_HEALTH_MS));
    }
}

void health_task_create(void)
{
    TaskHandle_t h = xTaskCreateStatic(health_task, "health", HEALTH_STACK_WORDS,
                                       NULL, APP_PRIO_HEALTH, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
