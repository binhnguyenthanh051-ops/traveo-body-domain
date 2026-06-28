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

/* The handshake lives in its OWN section (.fbl_handshake), pinned by app_cm4.ld
 * at the shared address (noinit.ld) — separate from the general .noinit that the
 * BSP/libraries use, so its address can't drift when their .noinit changes. Same
 * address as the FBL's region; NOLOAD so startup never zeroes it. */
static fbl_handshake_t g_handshake __attribute__((section(".fbl_handshake")));

fbl_handshake_t *app_port_noinit(void)
{
    return &g_handshake;
}

void app_port_system_reset(void)
{
    __DSB();              /* ensure the .noinit store has landed */
    NVIC_SystemReset();   /* does not return */
}
