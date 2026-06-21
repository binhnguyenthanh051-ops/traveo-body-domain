/*
 * port_can.c — knock-frame poll (target).  STUB (board-gated).
 *
 * TODO(M1, board): bring up CAN as the bootloader variant of shared/can
 * (polled + minimal static config, ADR-0004) and poll for the knock frame on
 * FBL_KNOCK_CAN_ID (config). In M3 this hook becomes a real UDS
 * DiagnosticSessionControl check — the core does not change.
 *
 * Safe default: no contact => the knock window always times out => the FBL
 * proceeds to the normal boot decision.
 */
#include "fbl_port.h"

bool fbl_port_tool_contact(void)
{
    return false;
}
