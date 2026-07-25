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

#define HEARTBEAT_STACK_WORDS   configMINIMAL_STACK_SIZE
#define HEARTBEAT_PRIORITY      (tskIDLE_PRIORITY + 1U)
#define HEARTBEAT_PERIOD_MS     500U

static StaticTask_t s_hb_tcb;
static StackType_t  s_hb_stack[HEARTBEAT_STACK_WORDS];

static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

static void heartbeat_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
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

    TaskHandle_t hb = xTaskCreateStatic(heartbeat_task,
                                        "hb",
                                        HEARTBEAT_STACK_WORDS,
                                        NULL,
                                        HEARTBEAT_PRIORITY,
                                        s_hb_stack,
                                        &s_hb_tcb);
    configASSERT(hb != NULL);

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
