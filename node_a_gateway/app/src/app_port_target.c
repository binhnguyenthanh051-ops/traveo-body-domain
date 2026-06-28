/*
 * app_port_target.c — ModusToolbox implementation of app_port.h (target-only).
 *
 * The .noinit handshake object is placed in the pinned .noinit section (ADR-0007
 * D8); the linker fixes it at the address both images agree on, and startup must
 * not zero it. The barrier before the reset guarantees the .noinit write is
 * visible to the FBL after the reset (ADR-0007 D7).
 */
#include "app_port.h"
#include "cy_pdl.h"   /* CMSIS core: __DSB, NVIC_SystemReset */

/* Pinned, not zeroed by startup. Same address as the FBL's region (noinit.ld). */
static fbl_handshake_t g_handshake __attribute__((section(".noinit")));

fbl_handshake_t *app_port_noinit(void)
{
    return &g_handshake;
}

void app_port_system_reset(void)
{
    __DSB();              /* ensure the .noinit store has landed */
    NVIC_SystemReset();   /* does not return */
}
