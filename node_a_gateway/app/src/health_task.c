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
#include "cy_pdl.h"   /* Cy_GPIO_* */

#define HEALTH_STACK_WORDS   256U   /* generous for bring-up; trim from high-water (ADR-0010 D5) */

/* App heartbeat = LED4 (P12.2) on the CYTVII-B-E-1M-SK (kit guide). Deliberately
 * NOT the FBL's LED (P19.0), so app-blink vs FBL-blink is visible at a glance.
 * Configured directly (strong drive), matching the FBL's port_prog.c style. */
#define APP_LED_PORT   GPIO_PRT12
#define APP_LED_PIN    2U

static StaticTask_t s_tcb;
static StackType_t  s_stack[HEALTH_STACK_WORDS];

static void health_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;)
    {
        Cy_GPIO_Inv(APP_LED_PORT, APP_LED_PIN);   /* heartbeat — slower than the FBL's */
        /* TODO(bring-up): once all tasks are up, sample
         * uxTaskGetStackHighWaterMark per task and record the minima for the
         * stack-trim pass. */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(APP_PERIOD_HEALTH_MS));
    }
}

void health_task_create(void)
{
    Cy_GPIO_Pin_FastInit(APP_LED_PORT, APP_LED_PIN,
                         CY_GPIO_DM_STRONG_IN_OFF, 0U, HSIOM_SEL_GPIO);

    TaskHandle_t h = xTaskCreateStatic(health_task, "health", HEALTH_STACK_WORDS,
                                       NULL, APP_PRIO_HEALTH, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
