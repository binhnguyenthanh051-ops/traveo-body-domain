/*
 * fbl_diag.c — M3 Seam 4/5: the real UDS diagnostic stack, composition root.
 *
 * The static dispatch table (ADR-0012 D3) names concrete handler modules --
 * this file is the one place allowed to (ADR-0012 D1: session depends on the
 * handler *interface*, not concrete handlers by name; the composition root
 * wires them). Scope so far: 0x10 DiagnosticSessionControl, 0x11 ECUReset
 * (Seam 4), 0x27 SecurityAccess (Seam 5) -- download/routineControl land in
 * later seams once flash is up.
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
#include "fbl_config.h"    /* FBL_DIAG_RESPONSE_ID */
#include "isotp.h"
#include "uds_session.h"
#include "uds_session_control.h"
#include "uds_ecu_reset.h"
#include "uds_security_access.h"
#include "boot.h"
#include "fbl_port.h"
#include "cy_pdl.h"        /* __DSB */

static const uds_handler_if_t *g_table[3];

void fbl_diag_init(void)
{
    isotp_init(fbl_can_hal(), FBL_DIAG_RESPONSE_ID);

    g_table[0] = uds_session_control_handler();
    g_table[1] = uds_ecu_reset_handler();
    g_table[2] = uds_security_access_handler();
    uds_session_init(g_table, 3U);
}

void fbl_diag_tick(uint32_t now_ms)
{
    uds_session_event_t event = uds_session_tick(now_ms);

    if (event == UDS_SESSION_EVENT_RESET_REQUESTED)
    {
        fbl_handshake_t h;
        boot_handshake_encode(&h, FBL_BOOT_APP);
        *fbl_port_noinit() = h;
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
