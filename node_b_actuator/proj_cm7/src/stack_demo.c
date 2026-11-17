/*
 * stack_demo.c — a small call chain to look at with the debugger (bench only).
 *
 * Built only with STACK_DEMO=1 or STACK_DEMO=2 on the make command line
 * (docs/hardware/kits.md "Bring-up switches"); the normal image has none of it.
 * It exists so the stack blog post can show a real stack, word by word.
 *
 * A dedicated task calls func_a -> func_b -> func_c. Each function keeps a small
 * array of easy-to-spot values (0xAAAA...., 0xBBBB...., 0xCCCC....) on its stack
 * and hands it to stack_demo_mark(). That function does nothing useful: it is the
 * place to put the breakpoint, hit once per step, with the step in r0:
 *
 *   1  the task is about to call func_a
 *   2  inside func_a        3  inside func_b        4  inside func_c (deepest)
 *   5  back in the task after all three returned: the old values are still there
 *
 * The task waits until the debugger sets g_stack_demo_go, runs the chain once,
 * and waits again. So the debugger attaches to a running board, sets its
 * breakpoint, then lets it go: no reset needed (tools/bench/stack_demo_capture.gdb).
 * It waits in a plain loop, not vTaskDelay(), so no call leaves frames behind
 * before the first step; only the tick interrupt's frame does, which is real.
 * It runs at the idle priority, so it takes CPU time from the idle task only.
 *
 * STACK_DEMO=2 (the overflow): func_c's array is bigger than the task's whole
 * stack. It spills below the stack into s_demo_spill, and func_c then yields, so
 * FreeRTOS's overflow check (method 2) runs at the task switch and calls
 * vApplicationStackOverflowHook(). Step 4 is reached; step 5 never is.
 *
 * MISRA: CY_NOINLINE (a GCC attribute) keeps each function a real call with its
 * own frame; without it the compiler could merge the chain and there would be
 * nothing to show. Bench-only code, deliberate.
 */
#include "stack_demo.h"

#if defined(STACK_DEMO) && ((STACK_DEMO == 1) || (STACK_DEMO == 2))

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "cy_utils.h"      /* CY_NOINLINE */

#define STACK_DEMO_WORDS     128U    /* 512 B: small enough to dump in full */
#define STACK_DEMO_PRIORITY  (tskIDLE_PRIORITY)

#if (STACK_DEMO == 2)
#define ARR_C_WORDS          160U    /* 640 B: more than the whole 512 B stack */
#else
#define ARR_C_WORDS          4U
#endif

/* Defined in this order on purpose: in this image a later static lands at a
 * lower address, so s_demo_spill sits right below the stack, where an overflow
 * goes (checked with arm-none-eabi-nm -n after the build). */
static StaticTask_t s_demo_tcb;
static StackType_t  s_demo_stack[STACK_DEMO_WORDS];
static volatile uint32_t s_demo_spill[128];

/* Set to 1 by the debugger to run the chain once; the task clears it. */
volatile uint32_t g_stack_demo_go;
/* The last step reached (see the header comment). */
volatile uint32_t g_stack_demo_step;
static volatile uint32_t s_demo_sink;

/* Reads the array, so the compiler must really build it and keep it until here.
 * Calls nothing, but still pushes the r4 it borrows (push {r4}, seen on the
 * bench 2026-11-16): that push lands in the popped area and shows in step 5. */
static CY_NOINLINE void stack_demo_keep(const uint32_t *arr, uint32_t n)
{
    uint32_t sum = 0U;

    for (uint32_t i = 0U; i < n; i++)
    {
        sum += arr[i];
    }
    s_demo_sink = sum;
}

/* The breakpoint goes here, on its FIRST instruction (break *stack_demo_mark):
 * it pushes r4, r5 right after, and stopping before that shows the callers'
 * stack untouched. */
static CY_NOINLINE void stack_demo_mark(uint32_t step, const uint32_t *arr, uint32_t n)
{
    uint32_t sum = 0U;

    for (uint32_t i = 0U; i < n; i++)
    {
        sum += arr[i];
    }
    s_demo_sink = sum;
    g_stack_demo_step = step;
}

static CY_NOINLINE void func_c(uint32_t x)
{
    uint32_t arr_c[ARR_C_WORDS];

    for (uint32_t i = 0U; i < ARR_C_WORDS; i++)
    {
        arr_c[i] = 0xCCCC0001U + i;
    }
    arr_c[ARR_C_WORDS - 1U] = x;          /* the argument, stored where it can be seen */
    stack_demo_mark(4U, arr_c, ARR_C_WORDS);
#if (STACK_DEMO == 2)
    taskYIELD();                          /* a task switch runs the overflow check */
#endif
}

static CY_NOINLINE void func_b(uint32_t x)
{
    uint32_t arr_b[2] = { 0xBBBB0001U, 0xBBBB0002U };

    stack_demo_mark(3U, arr_b, 2U);
    func_c(x + 0x11U);
    stack_demo_keep(arr_b, 2U);           /* keeps arr_b alive across the call */
}

static CY_NOINLINE void func_a(uint32_t x)
{
    uint32_t arr_a[2] = { 0xAAAA0001U, 0xAAAA0002U };

    stack_demo_mark(2U, arr_a, 2U);
    func_b(x + 0x11U);
    stack_demo_keep(arr_a, 2U);           /* keeps arr_a alive across the call */
}

static void stack_demo_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        while (g_stack_demo_go == 0U)
        {
            /* wait for the debugger: no call, so no frame */
        }
        stack_demo_mark(1U, NULL, 0U);
        func_a(0x11U);
        stack_demo_mark(5U, NULL, 0U);
        g_stack_demo_go = 0U;
        s_demo_spill[0] = s_demo_sink;    /* keeps the landing zone in the image */
    }
}

void stack_demo_create(void)
{
    TaskHandle_t h = xTaskCreateStatic(stack_demo_task, "stackdemo", STACK_DEMO_WORDS,
                                       NULL, STACK_DEMO_PRIORITY, s_demo_stack, &s_demo_tcb);
    configASSERT(h != NULL);
}

#endif /* STACK_DEMO */
