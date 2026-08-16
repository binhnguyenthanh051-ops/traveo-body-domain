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
#include "timers.h"   /* xTimerGetTimerDaemonTaskHandle */
#include "cybsp.h"
#include "cy_pdl.h"   /* Cy_GPIO_* */
#include "tb_log.h"     /* log_evt (ADR-0023) */
#include "log_events.h"  /* LOG_EVT_APP_ALIVE — generated from events.csv */

#define HEALTH_STACK_WORDS   128U   /* used ~26 words (g_hw_health) + margin (ADR-0010 D5) */

/* Bring-up: free stack words (min ever) per task — read in the debugger to size
 * the stacks (ADR-0010 D5). Small value = close to overflow. */
volatile UBaseType_t g_hw_health;
volatile UBaseType_t g_hw_idle;
volatile UBaseType_t g_hw_timer;

/* App heartbeat = LED4 (P12.2) on the CYTVII-B-E-1M-SK (kit guide). Deliberately
 * NOT the FBL's LED (P19.0), so app-blink vs FBL-blink is visible at a glance.
 * Configured directly (strong drive), matching the FBL's port_prog.c style. */
#define APP_LED_PORT   GPIO_PRT12
#define APP_LED_PIN    2U

static StaticTask_t s_tcb;
static StackType_t  s_stack[HEALTH_STACK_WORDS];

/* -------------------------------------------------------------------
 * Liveness cadence (ADR-0023 / REQ-LOG-009)
 *
 * One rate: every ALIVE_PERIOD_MS, with the FIRST emission immediate.
 *
 * A liveness event only ever says "still here", so a fast rate buys nothing
 * that LOG_EVT_BOOT and the banner do not already state outright -- it just
 * scrolls the events that carry information off the screen.
 *
 * The first one is NOT delayed, deliberately. "Every 5 s" implemented as
 * wait-then-emit would leave a 5 s blind window after every reset, and the BVT
 * power-cycles between every test: liveness evidence has to arrive at t~0, not
 * one period later.
 *
 * NOTE FOR THE BENCH: the cadence is part of the contract, because the BVT
 * liveness test keys on this event. Any timeout it uses must clear
 * ALIVE_PERIOD_MS with margin.
 *
 * Throttling lives HERE, at the call site, not in shared/log. How often a
 * caller has something worth saying is the caller's business; the channel's job
 * is to carry it (ADR-0023 D7).
 * ----------------------------------------------------------------- */
#define ALIVE_PERIOD_MS  5000U

static bool       s_alive_started;
static TickType_t s_alive_last;

/* @impl REQ-LOG-009 : the BVT liveness test (bench test 1) keys on this.
 * arg1 carries the health task's own stack headroom -- free to send, and it
 * turns "the node is alive" into "the node is alive AND not about to overflow a
 * stack", which is the failure this task exists to catch but could previously
 * only report to a debugger. */
static void health_log_alive(void)
{
    const TickType_t now = xTaskGetTickCount();

    /* Unsigned tick subtraction is wrap-safe; (now >= last + period) is not.
     * The started flag is what makes the first emission immediate -- zeroed
     * statics alone would make it wait a full period. */
    if (s_alive_started && ((now - s_alive_last) < pdMS_TO_TICKS(ALIVE_PERIOD_MS)))
    {
        return;
    }

    log_evt(LOG_EVT_APP_ALIVE,
            (uint32_t)((uint32_t)now * (uint32_t)portTICK_PERIOD_MS),
            (uint16_t)g_hw_health);

    s_alive_last    = now;
    s_alive_started = true;
}

static void health_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;)
    {
        Cy_GPIO_Inv(APP_LED_PORT, APP_LED_PIN);   /* heartbeat — slower than the FBL's */

        /* Stack high-water for this task + the two FreeRTOS-owned tasks (the app
         * tasks report their own into g_hw_app / g_hw_can). */
        g_hw_health = uxTaskGetStackHighWaterMark(NULL);
        g_hw_idle   = uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle());
        g_hw_timer  = uxTaskGetStackHighWaterMark(xTimerGetTimerDaemonTaskHandle());

        health_log_alive();

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
