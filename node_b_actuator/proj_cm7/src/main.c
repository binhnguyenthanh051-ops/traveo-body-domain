/*
 * main.c — Node B CM7 application entry.
 *
 * The CM0+ image performs BSP-wide configuration before releasing CM7_0, so the
 * CM7 side only needs to start the RTOS task set for this seam.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "cycfg_pins.h"
#include "cy_gpio.h"

#define APP_TASK_STACK_WORDS    (configMINIMAL_STACK_SIZE)
#define APP_TASK_PRIORITY       (tskIDLE_PRIORITY + 1U)
#define APP_BLINK_PERIOD_MS     500U

static StaticTask_t s_app_task_tcb;
static StackType_t  s_app_task_stack[APP_TASK_STACK_WORDS];

static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

static void app_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(APP_BLINK_PERIOD_MS));
    }
}

int main(void)
{
    TaskHandle_t task = xTaskCreateStatic(app_task,
                                          "actuator",
                                          APP_TASK_STACK_WORDS,
                                          NULL,
                                          APP_TASK_PRIORITY,
                                          s_app_task_stack,
                                          &s_app_task_tcb);

    configASSERT(task != NULL);

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
