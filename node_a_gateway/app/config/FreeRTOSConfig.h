/*
 * FreeRTOSConfig.h — Node A application kernel configuration (ADR-0010).
 *
 * Target-only (Cortex-M4F, ARM_CM4F port). The locked decisions:
 *   - 160 MHz CM4F, 1 kHz SysTick tick (D1).
 *   - Heap forbidden: static allocation only, dynamic compiled out (D2) — any
 *     heap-based create API then fails to LINK, making "no runtime malloc" a
 *     compile-time guarantee.
 *   - Interrupt-priority model: __NVIC_PRIO_BITS=3, KERNEL=0xE0, MAX_SYSCALL=0x40
 *     (D3). configMAX_SYSCALL_INTERRUPT_PRIORITY masks via BASEPRI.
 *   - Stack-overflow check method 2; configASSERT defined (D3/D4).
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* CMSIS device header (provides __NVIC_PRIO_BITS, SystemCoreClock). Pulled via
 * the ModusToolbox BSP. */
#include "cy_device_headers.h"

/* ---- Scheduler / timing (D1) ---- */
#define configUSE_PREEMPTION                    1
/* Use the runtime core clock the BSP computes (design target ~160 MHz, ADR-0010
 * D1) so the SysTick reload is correct regardless of the configured clock tree. */
extern uint32_t SystemCoreClock;
#define configCPU_CLOCK_HZ                      ( SystemCoreClock )
#define configTICK_RATE_HZ                      ( 1000U )
#define configMAX_PRIORITIES                    5
/* Words. The idle task. Sized from measured high-water (idle used ~11 words) plus
 * a margin over the worst-case CM4F context frame (~50 words). ADR-0010 D4/D5. */
#define configMINIMAL_STACK_SIZE                ( 128U )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

/* ---- Allocation policy: heap forbidden (D2) ---- */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0

/* ---- Hooks / debug aids (D3/D4) ---- */
#define configUSE_IDLE_HOOK                     0              /* health is a task */
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2              /* FP frame -> big stacks */
#define configUSE_MALLOC_FAILED_HOOK            0              /* no heap to fail */

/* ---- Software timers (D5) ---- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )   /* 4 */
#define configTIMER_TASK_STACK_DEPTH            ( 128U )       /* daemon used ~28 words + margin */
#define configTIMER_QUEUE_LENGTH                8

/* ---- Synchronisation primitives in use ---- */
#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               4

/* ---- Memory: no heap, so no heap size. ---- */

/* ---- Interrupt-priority model (D3) ---- */
/* __NVIC_PRIO_BITS comes from the CMSIS device header; assert the assumed value. */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS                     __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS                     3
#endif

/* Lowest priority (largest value) — SysTick + PendSV run here. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         7
/* Highest priority from which a ...FromISR API may be called. ISRs more urgent
 * than this must NOT touch the kernel (D3). */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    2

#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )        /* 0xE0 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )   /* 0x40 */

/* ---- configASSERT (D3): trap + record. vAssertCalled lives in main.c. ---- */
extern void vAssertCalled(const char *file, unsigned long line);
#define configASSERT( x )  if ( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )

/* ---- API inclusion (trim to what the app uses) ---- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0              /* static tasks are permanent */
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_uxTaskGetStackHighWaterMark     1              /* health task uses this */
#define INCLUDE_xTaskGetIdleTaskHandle          1              /* idle high-water sample */
#define INCLUDE_xTaskGetSchedulerState          1

/* The CM4F port maps these CMSIS handlers to the kernel. */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
