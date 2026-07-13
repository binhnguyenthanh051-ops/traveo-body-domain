/*
 * port_reset.c — reset-cause read/clear (target).
 *
 * Maps the PDL reset reason (Cy_SysLib_GetResetReason) to fbl_reset_cause_t per
 * ADR-0007 D2. The CM4 system init / cybsp_init do NOT clear the reason, so it
 * still reflects the real cause when fbl_run_boot() reads it.
 *
 * RES_CAUSE bit-field (TRAVEO T2G TVII-B-E-1M, reg 0x4026_1800, default
 * 0x4000_0000 — see docs/references/Reg_ResetCause.png):
 *   - POR is bit 30 (RESET_PORVDDD) — NOT reason == 0 (the device's default has
 *     bit 30 set). Brown-out (BOD*) and external reset (XRES/PXRES) are likewise
 *     "the chip came up fresh" => POWER_ON (cold => prime .noinit, ADR-0007 D3).
 *   - Hibernate-wake is checked first; on this part the low-voltage cause bits
 *     re-assert on wakeup, so testing HIB_WAKEUP before the POR group keeps a
 *     wake from being misread as a plain power-on. (Hibernate is not exercised
 *     in M1; verify the wake source/RES_CAUSE2 path when it is.)
 */
#include "fbl_port.h"
#include "cy_pdl.h"

/* "Chip came up cold" — power-on / brown-out / external reset. */
#define FBL_RST_COLD_MASK   (CY_SYSLIB_RESET_PORVDDD |                          \
                             CY_SYSLIB_RESET_BODVDDD | CY_SYSLIB_RESET_BODVDDA | \
                             CY_SYSLIB_RESET_BODVCCD |                          \
                             CY_SYSLIB_RESET_XRES   | CY_SYSLIB_RESET_PXRES)

/* Hardware + multi-counter watchdog timeouts. */
#define FBL_RST_WDT_MASK    (CY_SYSLIB_RESET_HWWDT  |                           \
                             CY_SYSLIB_RESET_SWWDT0 | CY_SYSLIB_RESET_SWWDT1 |  \
                             CY_SYSLIB_RESET_SWWDT2 | CY_SYSLIB_RESET_SWWDT3)

fbl_reset_cause_t fbl_port_reset_cause(void)
{
    uint32_t reason = Cy_SysLib_GetResetReason();
    fbl_reset_cause_t cause;

    if ((reason & CY_SYSLIB_RESET_HIB_WAKEUP) != 0U)
    {
        cause = FBL_RST_HIBERNATE;
    }
    else if ((reason & FBL_RST_COLD_MASK) != 0U)
    {
        cause = FBL_RST_POWER_ON;                  /* POR / BOD / XRES (bit 30 = POR) */
    }
    else if ((reason & CY_SYSLIB_RESET_SOFT) != 0U)
    {
        cause = FBL_RST_SOFTWARE;                   /* SYSRESETREQ */
    }
    else if ((reason & FBL_RST_WDT_MASK) != 0U)
    {
        cause = FBL_RST_WATCHDOG;
    }
    else if ((reason & CY_SYSLIB_RESET_TC_DBGRESET) != 0U)
    {
        cause = FBL_RST_DEBUG;
    }
    else
    {
        cause = FBL_RST_OTHER;                      /* prime-biased default (D3) */
    }

    return cause;
}

void fbl_port_clear_reset_cause(void)
{
    Cy_SysLib_ClearResetReason();
}
