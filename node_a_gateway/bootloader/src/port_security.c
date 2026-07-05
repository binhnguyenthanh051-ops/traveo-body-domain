/*
 * port_security.c — device-lifecycle read (target).
 *
 * Reads CPUSS_PROTECTION.STATE (0x402020C4, bits [2:0] -- confirmed against
 * docs/references/CPUSS_Protection_Secure_State.png) and maps it to
 * fbl_lifecycle_t. This gates the knock window (ADR-0008 D2).
 *
 * The register has five raw states; fbl_lifecycle_t distinguishes four.
 * Mapping:
 *   UNKNOWN (0) -> FBL_LC_SECURE  (ambiguous -- deny the knock window; same
 *                                  prime-bias safe-default direction as
 *                                  ADR-0007 D3's reset-cause classification)
 *   VIRGIN  (1) -> FBL_LC_NORMAL  (unprovisioned is at least as open as NORMAL)
 *   NORMAL  (2) -> FBL_LC_NORMAL
 *   SECURE  (3) -> FBL_LC_SECURE
 *   DEAD    (4) -> FBL_LC_SECURE  (terminal state -- deny, not a case that
 *                                  should ever legitimately reach the FBL)
 *
 * M1 defaulted this to a hardcoded FBL_LC_SECURE stub simply because nothing
 * needed the knock window to actually open yet. Per ADR-0008 D5's own design
 * intent -- "develop in the open (pre-SECURE) lifecycle so the board stays
 * reflashable; the knock window is the convenience this buys" -- a dev board
 * is supposed to sit in NORMAL lifecycle through M1-M4, not SECURE. This
 * board has never been lifecycle-provisioned (that step is still pending,
 * ADR-0008 D5), so it reads NORMAL/VIRGIN here, not SECURE.
 */
#include "fbl_port.h"
#include "cy_pdl.h"

fbl_lifecycle_t fbl_port_lifecycle(void)
{
    uint32_t state = _FLD2VAL(CPUSS_V2_PROTECTION_STATE, CPUSS->PROTECTION);
    fbl_lifecycle_t lc;

    switch (state)
    {
        case 1U:                 /* VIRGIN */
        case 2U:                 /* NORMAL */
            lc = FBL_LC_NORMAL;
            break;
        case 3U:                 /* SECURE */
            lc = FBL_LC_SECURE;
            break;
        case 0U:                 /* UNKNOWN */
        case 4U:                 /* DEAD */
        default:
            lc = FBL_LC_SECURE;   /* ambiguous/terminal -- deny, safe default */
            break;
    }

    return lc;
}
