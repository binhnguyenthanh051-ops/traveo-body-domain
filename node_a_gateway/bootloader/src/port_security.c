/*
 * port_security.c — device-lifecycle read (target).  STUB (board-gated).
 *
 * TODO(M1, board): read the device lifecycle stage (eFuse / SFLASH) and map it
 * to fbl_lifecycle_t. This gates the knock window (ADR-0008 D2).
 *
 * Safe default: FBL_LC_SECURE — this keeps the dev knock backdoor CLOSED until a
 * real lifecycle read is in place. A stub that returned a pre-SECURE stage would
 * leave the backdoor open by default, which is the wrong failure direction.
 * Implement this (returning the true stage) before the knock window can be used
 * on the bench.
 */
#include "fbl_port.h"

fbl_lifecycle_t fbl_port_lifecycle(void)
{
    return FBL_LC_SECURE;
}
