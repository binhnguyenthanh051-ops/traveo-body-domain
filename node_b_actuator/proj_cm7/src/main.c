/*
 * main.c — Node B CM7 application entry.
 *
 * The CM0+ image performs BSP-wide configuration (incl. GPIO pin init) before
 * releasing CM7_0, so the CM7 side only starts the RTOS. A dedicated heartbeat
 * task blinks User LED 1 (P5.0) as a liveness indicator — a blocked or starved
 * scheduler stops the blink.
 *
 * NOTE: the blink period rides the RTOS tick, so it is only as accurate as the
 * tick. While the CM7 SysTick/core-clock rate is still being sorted, this blinks
 * at the same (currently off) rate — the fix for that is upstream (the core
 * clock), not in this task.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "cybsp.h"
#include "cycfg_pins.h"
#include "cy_gpio.h"
#include "can_task.h"
#include "port_crypto.h"   /* secoc_crypto_port_init — MPU non-cacheable + offload bind */
#include "port_log.h"      /* log_port_init — SCB0 UART sink (ADR-0023) */
#include "tb_log.h"        /* log_evt */
#include "log_events.h"    /* LOG_EVT_APP_ALIVE — generated from events.csv */

#define HEARTBEAT_STACK_WORDS   configMINIMAL_STACK_SIZE
#define HEARTBEAT_PRIORITY      (tskIDLE_PRIORITY + 1U)
#define HEARTBEAT_PERIOD_MS     500U

static StaticTask_t s_hb_tcb;
static StackType_t  s_hb_stack[HEARTBEAT_STACK_WORDS];

static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

/* -------------------------------------------------------------------
 * Liveness cadence (ADR-0023 / REQ-LOG-009) — mirrors Node A's health_task.
 *
 * One rate, first emission immediate. "Every 5 s" implemented as
 * wait-then-emit would leave a 5 s blind window after every reset, and the BVT
 * power-cycles between every test — liveness evidence must arrive at t~0.
 *
 * On THIS node the cadence matters more than on Node A: the BVT's SecOC
 * replay/forgery tests assert the actuator HELD its state, and a liveness
 * event is what separates "correctly refused" from "dead board". Any bench
 * timeout must clear ALIVE_PERIOD_MS with margin.
 * ----------------------------------------------------------------- */
#define ALIVE_PERIOD_MS  5000U

static bool       s_alive_started;
static TickType_t s_alive_last;

/* @impl REQ-LOG-009 : the BVT liveness test keys on this. arg1 carries this
 * task's stack headroom -- free to send, and it upgrades "the node is alive"
 * to "alive AND not about to overflow a stack". */
static void heartbeat_log_alive(void)
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
            (uint16_t)uxTaskGetStackHighWaterMark(NULL));

    s_alive_last    = now;
    s_alive_started = true;
}

static void heartbeat_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
        heartbeat_log_alive();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

int main(void)
{
    /* SystemCoreClockUpdate() returns 0 on the CM7: the clock init (init_cycfg_system)
     * runs on the CM0+, and the PDL's frequency bookkeeping is per-core, so the CM7's
     * globals stay 0 and the FreeRTOS SysTick reload is garbage. The only value the
     * PDL cannot recover on its own is the ECO *crystal* frequency — a passive board
     * part with no register to read, so software must state it (this is what
     * Cy_SysClk_EcoConfigure did on the CM0+). Seed just that board fact (16 MHz =
     * CY_CFG_SYSCLK_ECO_FREQ), then let SystemCoreClockUpdate() DERIVE SystemCoreClock
     * and cy_AhbFreqHz from the real tree (CLK_FAST_0 <- CLKHF0 <- CLKPATH3 <-
     * PLL_200M_0) — no hardcoded core frequency. */
    Cy_SysClk_EcoSetFrequency(16000000UL);
    SystemCoreClockUpdate();

    /* Observability, immediately after the clock tree is known to THIS core and
     * before anything that might have something to report (ADR-0023). Order is
     * load-bearing: the PDL's frequency bookkeeping is per-core, so before
     * SystemCoreClockUpdate() the peripheral-clock read returns 0, the baud
     * setup correctly refuses, and the console would stay silent for the whole
     * run. Emits the banner + LOG_EVT_BOOT; touches no FreeRTOS object, so it is
     * safe this side of the scheduler. */
    log_port_init();

    /* Crypto-offload seam (ADR-0018 D6 / ADR-0021): program the MPU non-cacheable
     * region over the cross-core mailbox, idle the mailbox, and bind the IPC
     * transport into crypto_service so tasks can call crypto_mac(). Must precede
     * any crypto_mac() call; MPU setup is independent of the scheduler. */
    secoc_crypto_port_init();

    TaskHandle_t hb = xTaskCreateStatic(heartbeat_task,
                                        "hb",
                                        HEARTBEAT_STACK_WORDS,
                                        NULL,
                                        HEARTBEAT_PRIORITY,
                                        s_hb_stack,
                                        &s_hb_tcb);
    configASSERT(hb != NULL);

    /* Lowest-priority drain: moves queued log bytes to the UART (ADR-0023 D10).
     * Created before can_task so a failure in CAN bring-up still gets reported. */
    log_task_create();

    /* Bring up CANFD + create the CAN RX/TX task (Phase A: internal loopback).
     * The actuator FSM task joins here in a later seam. */
    can_task_create();

    vTaskStartScheduler();

    for (;;)
    {
        /* Scheduler start should not return. */
    }
}

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxTcb,
                                   StackType_t **ppxStack,
                                   uint32_t *pulStackSize)
{
    *ppxTcb = &s_idle_tcb;
    *ppxStack = s_idle_stack;
    *pulStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTcb,
                                    StackType_t **ppxStack,
                                    uint32_t *pulStackSize)
{
    *ppxTcb = &s_timer_tcb;
    *ppxStack = s_timer_stack;
    *pulStackSize = configTIMER_TASK_STACK_DEPTH;
}

volatile TaskHandle_t g_overflow_task;
volatile const char  *g_overflow_task_name;

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    g_overflow_task = xTask;
    g_overflow_task_name = pcTaskName;
    taskDISABLE_INTERRUPTS();

    for (;;)
    {
        /* Trap in debugger. */
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();

    for (;;)
    {
        /* Trap in debugger. */
    }
}
