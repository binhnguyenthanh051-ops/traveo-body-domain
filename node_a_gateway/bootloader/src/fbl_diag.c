/*
 * fbl_diag.c — M3: the real UDS diagnostic stack, composition root.
 *
 * The static dispatch table (ADR-0012 D3) names concrete handler modules --
 * this file is the one place allowed to (ADR-0012 D1: session depends on the
 * handler *interface*, not concrete handlers by name; the composition root
 * wires them). Full M3 service set:
 *   0x10 DiagnosticSessionControl (Seam 4)  0x27 SecurityAccess (Seam 5)
 *   0x11 ECUReset (Seam 4)                  0x34/0x36/0x37 download (Seam 7)
 *                                           0x31 RoutineControl erase+verify (Seam 7)
 *
 * The download and routineControl handlers are bound to the FBL's flash port
 * (fbl_flash_hal(), Seam 6) and the app-image region (fbl_port_app_*) here,
 * keeping shared/diag itself flash- and image-agnostic.
 *
 * On UDS_SESSION_EVENT_RESET_REQUESTED, performs the FBL's own reset
 * sequence (ADR-0007 D10): clear the .noinit programming-request to
 * FBL_BOOT_APP before resetting, unconditionally, regardless of whether a
 * download actually happened -- the existing boot-decision fail-safe (not a
 * new mechanism here) is what actually decides whether the app is valid
 * enough to jump to on the next boot.
 */
#include "fbl_diag.h"
#include "fbl_can.h"
#include "fbl_flash.h"
#include "fbl_config.h"    /* FBL_DIAG_RESPONSE_ID */
#include "isotp.h"
#include "uds_session.h"
#include "uds_session_control.h"
#include "uds_ecu_reset.h"
#include "uds_security_access.h"
#include "uds_download.h"
#include "uds_routine_control.h"
#include "boot.h"
#include "fbl_port.h"
#include "cy_pdl.h"        /* __DSB */

static const uds_handler_if_t *g_table[7];

void fbl_diag_init(void)
{
    isotp_init(fbl_can_hal(), FBL_DIAG_RESPONSE_ID);

    /* Bind the operations-layer back ends to the target ports before the
     * handlers can run (ADR-0012 D6): the download writes and the routine
     * erases/verifies over the same app-image flash region. */
    uds_download_init(fbl_flash_hal());
    uds_routine_control_init(fbl_flash_hal(),
                             fbl_port_app_image_base(),
                             fbl_port_app_region_len());

    g_table[0] = uds_session_control_handler();
    g_table[1] = uds_ecu_reset_handler();
    g_table[2] = uds_security_access_handler();
    g_table[3] = uds_request_download_handler();
    g_table[4] = uds_transfer_data_handler();
    g_table[5] = uds_request_transfer_exit_handler();
    g_table[6] = uds_routine_control_handler();
    uds_session_init(g_table, 7U);
}

void fbl_diag_tick(uint32_t now_ms)
{
    uds_session_event_t event = uds_session_tick(now_ms);

    if (event == UDS_SESSION_EVENT_RESET_REQUESTED)
    {
        fbl_handshake_t h;
        boot_handshake_encode(&h, FBL_BOOT_APP);
        *fbl_port_noinit() = h;

        /* Clear the boot-loop counter (ADR-0007 D10 refinement, silicon-
         * verified during Seam 7 bring-up): an ECUReset is a deliberate,
         * tester-commanded reset -- exactly the "deliberate reflash is not a
         * crash" case D4 wants to clear. But D10 just cleared the .noinit
         * programming-request (above, so the post-reset boot can JUMP rather
         * than stay resident), which is the very signal D4 uses to recognise a
         * deliberate reset. So the counter would otherwise be incremented as
         * if this were a crash software-reset (boot.c), and repeated
         * download+reset cycles climbed it past the threshold, trapping a
         * valid freshly-flashed app in the FBL until a power cycle. Clearing
         * it here breaks that: the post-reset increment lands at 1 (< N), so
         * the boot decision reaches the app-validity check and jumps. */
        fbl_port_backup_write(FBL_BREG_COUNTER_IDX, 0U);

        __DSB();

        /* uds_session_tick() already called isotp_send() for the positive
         * response above, but that only requests the transmission -- it
         * does not wait for the frame to actually leave the TX buffer.
         * Without this, the reset can cut the response's transmission short
         * before it ever reaches the bus (silicon-verified: exactly this
         * symptom during Seam 4 bring-up). */
        fbl_can_wait_tx_complete();

        fbl_port_system_reset();   /* does not return */
    }
}
