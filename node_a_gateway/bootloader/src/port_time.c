/*
 * port_time.c — monotonic millisecond clock (target).
 *
 * SysTick at 1 kHz, the standard CMSIS pattern (SysTick_Config sets
 * LOAD/VAL and enables the exception + processor clock source).
 * SystemCoreClock is set by cybsp_init() -> SystemInit(), so fbl_time_init()
 * must run after it.
 *
 * port_jump.c already defensively zeroes SysTick before the app handover
 * (ADR-0008 D4 B3 contract) -- this is the first thing that actually starts
 * it, replacing the M1 per-call placeholder (real timing now needed for the
 * knock dwell, ADR-0008 D2, and ISO-TP's N_Cr timeout, ADR-0013 D3).
 */
#include "fbl_port.h"
#include "fbl_time.h"
#include "cy_pdl.h"     /* SysTick_Config, SystemCoreClock */

static volatile uint32_t s_tick_ms;

void SysTick_Handler(void)
{
    ++s_tick_ms;
}

void fbl_time_init(void)
{
    s_tick_ms = 0U;
    (void)SysTick_Config(SystemCoreClock / 1000U);
}

uint32_t fbl_port_now_ms(void)
{
    return s_tick_ms;
}
