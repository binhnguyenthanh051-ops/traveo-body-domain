/*
 * port_reset.c — reset-cause read/clear (target).  STUB (board-gated).
 *
 * TODO(M1, board): read the SRSS reset-cause register(s) (RES_CAUSE/RES_CAUSE2),
 * decode per ADR-0007 D2 — cause == 0 implies POR; map hibernate-wake and the
 * rest; clear (write-1-to-clear) after reading. Verify register semantics in TRM.
 *
 * Safe default until then: FBL_RST_OTHER, which is prime-biased (primes .noinit,
 * never opens the knock window, leaves the counter at the power-on path) — so the
 * FBL behaves safely even before this is implemented.
 */
#include "fbl_port.h"

fbl_reset_cause_t fbl_port_reset_cause(void)
{
    return FBL_RST_OTHER;
}

void fbl_port_clear_reset_cause(void)
{
    /* TODO(M1, board): write-1-to-clear the SRSS reset-cause register. */
}
