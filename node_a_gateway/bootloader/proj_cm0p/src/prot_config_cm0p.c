/*
 * prot_config_cm0p.c — CM0+ TCB-isolation wall (M4 Seam 5, ADR-0020).
 *
 * Behind FBL_M4_SEAM5_PROT (default OFF -> no-op, build unchanged from Seam 4).
 *
 * SCOPE (ADR-0020, settled on the bench): Boundary A — an SMPU that denies the
 * untrusted CM4 access to the CM0+ image flash (which holds the compiled-in
 * public key) — is the proven wall and the whole TCB-isolation claim: a CM4-side
 * read of that region takes a bus fault. Boundary B (a PPU over the CRYPTO block)
 * and D5 (locking the walls against CM4) are kept DESIGN-ONLY, behind default-off
 * flags, because CM4 never touches CRYPTO directly — all crypto crosses the IPC
 * mailbox to the CM0+ — so walling it is redundant defense-in-depth. M5's runtime
 * MAC key (which would live in the engine) is where the CRYPTO PPU earns its place.
 *
 * PC plan: on this BSP both CPUs boot in PC2 (the DAP is separately PC0). We leave
 * the CM0+ in PC2 and move ONLY the CM4 to an ordinary PC3, then deny PC3 the key
 * region with an SMPU in MATCH mode — so the struct matches only the CM4 and the
 * CM0+ falls through untouched (it keeps executing/reading its own flash). All of
 * this runs before Cy_SysEnableCM4 (ADR-0020 D2), while the CM4 is still in reset.
 */
#include "prot_config_cm0p.h"

#ifdef FBL_M4_SEAM5_PROT

#include "cy_pdl.h"          /* Cy_Prot_*, PROT_SMPU_SMPU_STRUCTn, CPUSS_MS_ID_* */
#include "prot_region.h"     /* host-tested descriptor math (ADR-0020 D6) */

/* Design-only walls, OFF by default (see SCOPE above). Flip on only to develop
 * them further; the proven demo needs neither. */
#define PROT_WALL_CRYPTO   0U   /* Boundary B: fixed PPUs over the CRYPTO block */
#define PROT_LOCK_MASTER   0U   /* D5: lock the SMPU pair against CM4 */

/* CM4's ordinary untrusted PC. MUST differ from the CM0+'s PC2 (the walls name
 * this PC; if it equalled the CM0+'s, the CM0+ would be walled out of its flash). */
#define PC_CM4_UNTRUSTED   3U

/* pcMask bit convention (cy_prot.h: CY_PROT_PCMASK1==0x0001 ...): bit (i-1) = PC i. */
#define PCMASK_FOR_PC(n)   ((uint16_t)(1U << ((n) - 1U)))
#define PCMASK_CM4         PCMASK_FOR_PC(PC_CM4_UNTRUSTED)                  /* PC3 -> 0x0004 */
#if PROT_WALL_CRYPTO
#define PCMASK_EXCEPT_CM4  ((uint16_t)(0x7FFFU & (uint16_t)~PCMASK_CM4))   /* every PC but CM4 */
#endif

/* CM0+ image flash extent holding the compiled-in public key (D3), from
 * fbl_cm4.ld: flash ORIGIN 0x1000_0000, FLASH_CM0P_SIZE 0x2_0000 (128 KB). */
#define KEY_REGION_BASE    0x10000000UL
#define KEY_REGION_LEN     0x00020000UL

/* Map the host-tested size_code onto the PDL enum (numerically equal). */
#define SIZE_CODE_TO_PDL(code)   ((cy_en_prot_size_t)(code))

/* ------------------------------------------------------------------ *
 * Boundary A — SMPU over the CM0+ image flash (the public key, D3).
 * ------------------------------------------------------------------ */
static void prot_setup_key_smpu(void)
{
    /* Region geometry from the HOST-TESTED helper (D6). For {0x1000_0000, 128 KB}:
     * base 0x1000_0000, size_code 16 (CY_PROT_SIZE_128KB), mask 0x00, exact. */
    prot_region_t rgn;
    if (!prot_region_cover(KEY_REGION_BASE, KEY_REGION_LEN, &rgn))
    {
        return;   /* impossible for a 32-bit-contained 128 KB region; fail closed */
    }

    /* MATCH mode (pcMatch=true): the struct participates ONLY for accesses whose
     * PC is in pcMask. With pcMask = CM4's PC alone it matches only the CM4
     * (DISABLED -> denied), while every other PC — including the CM0+'s PC2 — does
     * not match and falls through to the open background. So the CM0+ keeps
     * executing/reading its own flash and only the CM4 is walled out. */
    cy_stc_smpu_cfg_t deny_cm4 = {
        .address        = (uint32_t *)KEY_REGION_BASE,
        .regionSize     = SIZE_CODE_TO_PDL(rgn.size_code),
        .subregions     = rgn.subregion_disable,   /* 0x00 for the exact 128 KB */
        .userPermission = CY_PROT_PERM_DISABLED,
        .privPermission = CY_PROT_PERM_DISABLED,
        .secure         = false,
        .pcMatch        = true,                     /* MATCH mode */
        .pcMask         = PCMASK_CM4,               /* matches only CM4 (PC3) */
    };
    (void)Cy_Prot_ConfigSmpuSlaveStruct(PROT_SMPU_SMPU_STRUCT0, &deny_cm4);
    (void)Cy_Prot_EnableSmpuSlaveStruct(PROT_SMPU_SMPU_STRUCT0);

#if PROT_LOCK_MASTER
    /* D5 (design-only): lock the pair's own config against CM4, so a compromised
     * CM4 cannot disable the wall. Same match-mode trick — matches only CM4. */
    cy_stc_smpu_cfg_t lock = {
        .address        = (uint32_t *)0,
        .regionSize     = CY_PROT_SIZE_256B,        /* ignored for master; must be valid */
        .subregions     = 0U,
        .userPermission = CY_PROT_PERM_DISABLED,
        .privPermission = CY_PROT_PERM_DISABLED,
        .secure         = false,
        .pcMatch        = true,
        .pcMask         = PCMASK_CM4,
    };
    (void)Cy_Prot_ConfigSmpuMasterStruct(PROT_SMPU_SMPU_STRUCT0, &lock);
    (void)Cy_Prot_EnableSmpuMasterStruct(PROT_SMPU_SMPU_STRUCT0);
#endif
}

#if PROT_WALL_CRYPTO
/* ------------------------------------------------------------------ *
 * Boundary B (design-only) — fixed PPUs over the CRYPTO block (D4).
 *
 * The PPU ATT is a PER-PC permission table (unlike the SMPU's single ATT + PC
 * bitmask): pcMask selects which PCs' slots to overwrite, and PCs not named keep
 * their default. So the deny is two writes — grant every non-CM4 PC (incl. the
 * CM0+, which drives CRYPTO and would otherwise lose its slot), then DISABLE CM4.
 * NOTE: not bench-proven to fault a CM4 access — the six sub-block PPUs' fixed
 * region coverage of the probe address is unverified. Redundant with Boundary A
 * for M4 (CM4 never touches CRYPTO); brought up for real at M5.
 * ------------------------------------------------------------------ */
static void prot_setup_crypto_ppu(void)
{
    PERI_MS_PPU_FX_Type *const crypto_ppus[] = {
        PERI_MS_PPU_FX_CRYPTO_MAIN,  PERI_MS_PPU_FX_CRYPTO_CRYPTO,
        PERI_MS_PPU_FX_CRYPTO_BOOT,  PERI_MS_PPU_FX_CRYPTO_KEY0,
        PERI_MS_PPU_FX_CRYPTO_KEY1,  PERI_MS_PPU_FX_CRYPTO_BUF,
    };

    for (uint32_t i = 0U; i < (sizeof crypto_ppus / sizeof crypto_ppus[0]); ++i)
    {
        (void)Cy_Prot_ConfigPpuFixedSlaveAtt(crypto_ppus[i], PCMASK_EXCEPT_CM4,
                                             CY_PROT_PERM_RW, CY_PROT_PERM_RW, false);
        (void)Cy_Prot_ConfigPpuFixedSlaveAtt(crypto_ppus[i], PCMASK_CM4,
                                             CY_PROT_PERM_DISABLED, CY_PROT_PERM_DISABLED, false);
    }
}
#endif /* PROT_WALL_CRYPTO */

/* ------------------------------------------------------------------ *
 * PC assignment (D2) — move ONLY the CM4 to its untrusted PC3; leave the CM0+ in
 * its default PC2 untouched. CM4 is still in reset here (precedes Cy_SysEnableCM4).
 * ------------------------------------------------------------------ */
static void prot_assign_contexts(void)
{
    (void)Cy_Prot_ConfigBusMaster(CPUSS_MS_ID_CM4, true, false, PCMASK_CM4);  /* PC3 only, non-secure */
    (void)Cy_Prot_SetActivePC(CPUSS_MS_ID_CM4, PC_CM4_UNTRUSTED);
}

void cm0p_prot_install_walls(void)
{
    prot_setup_key_smpu();
#if PROT_WALL_CRYPTO
    prot_setup_crypto_ppu();
#endif
    prot_assign_contexts();
}

#else  /* !FBL_M4_SEAM5_PROT */

/* Walls disabled at this build: no-op so main_cm0p.c can call unconditionally
 * and the Seam-4 image is unchanged. */
void cm0p_prot_install_walls(void)
{
}

#endif /* FBL_M4_SEAM5_PROT */
