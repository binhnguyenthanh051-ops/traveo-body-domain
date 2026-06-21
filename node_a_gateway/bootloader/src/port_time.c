/*
 * port_time.c — monotonic millisecond clock (target).  STUB (board-gated).
 *
 * TODO(M1, board): drive from SysTick (or a free-running timer) at 1 kHz.
 *
 * The placeholder increments per call so the knock dwell always terminates even
 * before a real timer exists (it must never spin forever). This is NOT real
 * time — replace before relying on knock-window timing.
 */
#include "fbl_port.h"

uint32_t fbl_port_now_ms(void)
{
    static uint32_t ticks = 0U;
    ticks += 1U;
    return ticks;
}
