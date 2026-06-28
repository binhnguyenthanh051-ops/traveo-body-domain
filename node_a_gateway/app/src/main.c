/*
 * main.c — Node A application entry (ADR-0010).
 *
 * Target-only. Brings up the BSP, honours the app side of the FBL->app handover
 * contract (D7), creates the static task set, and starts the scheduler. The app
 * assumes nothing the FBL left: it does not re-enable interrupts here — the
 * ARM_CM4F port clears PRIMASK (cpsie i) when it starts the first task, so
 * interrupts come alive exactly at vTaskStartScheduler(), not before.
 *
 * Heap is forbidden (D2): with configSUPPORT_DYNAMIC_ALLOCATION=0 the kernel
 * needs the two static-memory callbacks below, and every task uses xTaskCreate
 * Static. There is no malloc-failed hook because there is no heap.
 */
#include "cybsp.h"
#include "cy_pdl.h"
#include "FreeRTOS.h"
#include "task.h"
#include "tasks.h"

/* App image base = the FBL's jump target (overview Sec.6 / ADR-0008). Used by a
 * cheap bring-up assert that the FBL set VTOR to us. */
#define APP_VECTOR_BASE   0x10040000UL

/* Enable the one-off handover sanity asserts during bring-up only. */
#ifndef M2_BRINGUP_ASSERTS
#define M2_BRINGUP_ASSERTS  1
#endif

static void verify_handover(void)
{
#if M2_BRINGUP_ASSERTS
    /* Trust the B3 contract, verify cheaply (D7): the FBL set VTOR to us, and
     * the CM0+ prebuilt disabled the WDT (M2-6). Not a runtime dependency. */
    configASSERT(SCB->VTOR == APP_VECTOR_BASE);
#endif
}

int main(void)
{
    cy_rslt_t result = cybsp_init();
    configASSERT(result == CY_RSLT_SUCCESS);
    (void)result;

    /* Defensive cross-image seam (D7): re-clear any stale SysTick pending bit the
     * FBL might have left. The FBL already clears it; this documents and
     * double-guards the assumption with a single register write. */
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    verify_handover();

    /* Create the static task set (ADR-0010 D5), then start scheduling. */
    health_task_create();
    can_task_create();
    app_task_create();

    vTaskStartScheduler();

    /* Unreachable: with no heap the scheduler cannot fail to start for want of
     * memory, and the tasks never return. */
    for (;;)
    {
        /* nothing */
    }
}

/* -------------------------------------------------------------------
 * Static-allocation callbacks (D2) — required because the heap is off.
 * ----------------------------------------------------------------- */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxTcb,
                                   StackType_t  **ppxStack,
                                   uint32_t      *pulStackSize)
{
    *ppxTcb       = &s_idle_tcb;
    *ppxStack     = s_idle_stack;
    *pulStackSize = configMINIMAL_STACK_SIZE;
}

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTcb,
                                    StackType_t  **ppxStack,
                                    uint32_t      *pulStackSize)
{
    *ppxTcb       = &s_timer_tcb;
    *ppxStack     = s_timer_stack;
    *pulStackSize = configTIMER_TASK_STACK_DEPTH;
}

/* -------------------------------------------------------------------
 * Kernel hooks (D3/D4).
 * ----------------------------------------------------------------- */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    /* TODO(bring-up): record the offending task, then safe-state. */
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
        /* trap */
    }
}

void vAssertCalled(const char *file, unsigned long line)
{
    (void)file;
    (void)line;
    /* TODO(bring-up): record file/line; halt for the debugger. */
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
        /* trap */
    }
}
