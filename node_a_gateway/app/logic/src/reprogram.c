/*
 * reprogram.c — App-side programming-request (ADR-0007 D7).
 *
 * Pure logic; host-testable. The data barrier before the reset lives in the
 * target app_port_system_reset() (it is hardware-specific), so this stays free
 * of vendor headers.
 */
#include "reprogram.h"
#include "app_port.h"
#include "boot.h"
#include "boot_types.h"

void app_request_reprogram(void)
{
    fbl_handshake_t *region = app_port_noinit();
    boot_handshake_encode(region, FBL_PROGRAMMING_REQUESTED);
    app_port_system_reset();   /* barrier + NVIC_SystemReset on target */
}
