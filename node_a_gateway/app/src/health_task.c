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

/* F-002: each of these off in FreeRTOSConfig.h only shows up as a linker error
 * on a full firmware build. Fail here instead, at compile time, naming the fix. */
#if (INCLUDE_uxTaskGetStackHighWaterMark != 1)
#error "health_task needs INCLUDE_uxTaskGetStackHighWaterMark 1 in FreeRTOSConfig.h"
#endif
#if (INCLUDE_xTaskGetIdleTaskHandle != 1)
#error "health_task needs INCLUDE_xTaskGetIdleTaskHandle 1 in FreeRTOSConfig.h"
#endif
#if (configUSE_TIMERS != 1)
#error "health_task needs configUSE_TIMERS 1 in FreeRTOSConfig.h (timer daemon high-water)"
#endif
#if (INCLUDE_xTaskGetHandle != 1)
#error "LOG_EVT_TASK_STACK needs INCLUDE_xTaskGetHandle 1 in FreeRTOSConfig.h (ADR-0025 D5)"
#endif

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
 * Per-task stack high-water (ADR-0025 D5, LOG_EVT_TASK_STACK)
 *
 * Right after each APP_ALIVE, one record per task: its id and its minimum free
 * stack words since boot. The bench check (bench_summary.py --stack-report)
 * asserts that the measured use never exceeds stack_bound's static worst case,
 * which is what shows the static number is an upper bound. The ids are the
 * contract in events.csv. Tasks are found by name, so one this image doesn't
 * create is skipped, not reported as zero.
 * ----------------------------------------------------------------- */
typedef struct
{
    uint32_t    id;
    const char *name;
} task_stack_src_t;

static const task_stack_src_t k_task_stacks[] = {
    { 1U, "can" }, { 2U, "app" }, { 3U, "health" }, { 4U, "LOG" },
    /* FreeRTOS's own names: the defaults of configIDLE_TASK_NAME / configTIMER_SERVICE_TASK_NAME,
     * which only tasks.c / timers.c can see. */
    { 6U, "IDLE" }, { 7U, "Tmr Svc" },
};

/* @impl REQ-LOG-016 : every task's stack high-water after each APP_ALIVE */
static void log_task_stacks(void)
{
    for (uint32_t i = 0U; i < (uint32_t)(sizeof k_task_stacks / sizeof k_task_stacks[0]); ++i)
    {
        TaskHandle_t h = xTaskGetHandle(k_task_stacks[i].name);
        if (h != NULL)
        {
            log_evt(LOG_EVT_TASK_STACK, k_task_stacks[i].id,
                    (uint16_t)uxTaskGetStackHighWaterMark(h));
        }
    }
}

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
    log_task_stacks();

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
