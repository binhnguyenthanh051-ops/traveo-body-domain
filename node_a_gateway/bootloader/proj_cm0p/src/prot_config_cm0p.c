/*
 * prot_config_cm0p.c — CM0+ TCB-isolation wall setup (M4 Seam 5, ADR-0020).
 *
 * SCAFFOLD. Every register-level fact is tagged TRM-VERIFY and the whole file is
 * behind FBL_M4_SEAM5_PROT (default OFF -> no-op, build unchanged from Seam 4).
 * Constants are read from the LOCAL PDL (mtb-pdl-cat1 release-v3.22.1,
 * COMPONENT_CAT1A, device CYT2B75CAS); runtime semantics are proven on the bench.
 *
 * PC plan (ADR-0020 D2, revised against the TRM protection-units chapter §6.3):
 * PC0 and PC1 are HARDWARE-SPECIAL, CM0+-only contexts (PC1 = trusted-ROM syscall
 * path, entered only on a trusted interrupt; PC0 = boot/manager, unrestricted).
 * CM4 can attain neither — they are CM0+-only. So:
 *   - CM0+ STAYS in its boot PC0 (unrestricted secure CPU). It reads the key and
 *     drives CRYPTO by PC0 bypass — no allow-rule, no self-brick. We do NOT touch
 *     CM0+'s MSx_CTL (that would risk clearing its PC0 permission).
 *   - CM4 is moved to ordinary PC2 with a mask of PC2-only, so it cannot program
 *     its way to PC0/PC1 (Door-1 clamp) and has no hardware path to them anyway.
 * The walls therefore only need to DENY CM4's PC:
 *   Boundary A (D3): one SMPU deny struct over the CM0+ image flash
 *     0x1000_0000..0x1002_0000 (128 KB, the compiled-in crypto_pubkey_dev[]).
 *   Boundary B (D4): the six FIXED CRYPTO PPUs, allowing no ordinary PC (PC0
 *     bypass still lets CM0+ in), so a CM4 (PC2) CRYPTO access faults.
 *   Lock (D5): master structs bound so only the PC0 secure CPU can rewrite them.
 */
#include "prot_config_cm0p.h"

#ifdef FBL_M4_SEAM5_PROT

#include "cy_pdl.h"          /* Cy_Prot_*, PROT_SMPU_SMPU_STRUCTn, PERI_MS_PPU_FX_CRYPTO_*, CPUSS_MS_ID_* */
#include "prot_region.h"     /* host-tested descriptor math (ADR-0020 D6) */

/* CM4's ordinary untrusted PC. CM0+ is left in its boot PC0 (see file header),
 * so there is no PC_CM0P define — CM0+ never appears in a wall's allow-mask; it
 * passes by the PC0 bypass the TRM grants unrestricted access. */
#define PC_CM4_UNTRUSTED   2U

/* pcMask bit convention (cy_prot.h: CY_PROT_PCMASK1==0x0001, PCMASK2==0x0002 ...):
 * bit (i-1) selects PC i. PC0 has NO bit here — it is the always-unrestricted
 * manager context. "Allow PC n" == (1u << (n-1)). */
#define PCMASK_FOR_PC(n)   ((uint16_t)(1U << ((n) - 1U)))

/* Deny every non-zero PC (PC1..PC15). Used as the SMPU deny struct's match mask:
 * CM4 (PC2) is caught; CM0+ (PC0) is never in this set and bypasses regardless. */
#define PCMASK_ALL_NONZERO ((uint16_t)0x7FFFU)

/* No ordinary PC (empty mask). For the CRYPTO PPU slave att and every master
 * (lock) struct: with 0x0000, only the PC0 bypass — i.e. the CM0+ secure CPU —
 * gets access / may reconfigure; CM4 (PC2) is denied. Valid per
 * CY_PROT_IS_PC_MASK_VALID (0 & ~MAX == 0). */
#define PCMASK_NONE        ((uint16_t)0x0000U)

/* CM0+ image flash extent holding the compiled-in public key (D3), from
 * fbl_cm4.ld: flash ORIGIN 0x1000_0000, FLASH_CM0P_SIZE 0x2_0000 (128 KB). */
#define KEY_REGION_BASE    0x10000000UL
#define KEY_REGION_LEN     0x00020000UL

/* Map the host-tested size_code onto the PDL enum (numerically equal — verified
 * in cy_prot.h: PROT_SIZE_256B_BIT_SHIFT==7, PROT_SIZE_128KB_BIT_SHIFT==16). */
#define SIZE_CODE_TO_PDL(code)   ((cy_en_prot_size_t)(code))

/* ------------------------------------------------------------------ *
 * Boundary A — SMPU over the CM0+ image flash (the public key, D3).
 *
 * One slave struct is enough now that CM0+ sits in the PC0 bypass: the struct
 * DENIES the whole 128 KB to every non-zero PC (so CM4's PC2 is refused), while
 * CM0+'s PC0 accesses skip SMPU evaluation entirely and keep read/execute.
 * ------------------------------------------------------------------ */
static void prot_setup_key_smpu(void)
{
    /* Geometry from the HOST-TESTED helper (D6). For {0x1000_0000, 128 KB}:
     * base 0x1000_0000, size_code 16 (CY_PROT_SIZE_128KB), mask 0x00, exact —
     * asserted by test_prot_region.c. */
    prot_region_t rgn;
    if (!prot_region_cover(KEY_REGION_BASE, KEY_REGION_LEN, &rgn))
    {
        return;   /* impossible for a 32-bit-contained 128 KB region; fail closed */
    }

    /* Slave: deny read/write/execute to all non-zero PCs over the key region.
     * pcMatch=0 (access evaluation): the DISABLED permission applies to any
     * access whose PC is in pcMask (PC1..15) — i.e. CM4. PC0 (CM0+) isn't in the
     * mask and bypasses anyway, so the trusted core is unaffected. */
    cy_stc_smpu_cfg_t deny = {
        .address        = (uint32_t *)KEY_REGION_BASE,
        .regionSize     = SIZE_CODE_TO_PDL(rgn.size_code),
        .subregions     = rgn.subregion_disable,      /* 0x00 for the exact 128 KB */
        .userPermission = CY_PROT_PERM_DISABLED,
        .privPermission = CY_PROT_PERM_DISABLED,
        .secure         = false,                       /* region attr; PC is the axis here */
        .pcMatch        = false,
        .pcMask         = PCMASK_ALL_NONZERO,
    };
    (void)Cy_Prot_ConfigSmpuSlaveStruct(PROT_SMPU_SMPU_STRUCT0, &deny);
    (void)Cy_Prot_EnableSmpuSlaveStruct(PROT_SMPU_SMPU_STRUCT0);

    /* D5 lock: the master sub-struct guards the pair's own config. Per the TRM
     * the master's READ is const-1, so this is WRITE-deny: with pcMask=NONE only
     * the PC0 bypass (CM0+) may rewrite/disable STRUCT0; CM4 (PC2) cannot. */
    cy_stc_smpu_cfg_t lock = {
        .address        = (uint32_t *)0,               /* unused by master struct */
        .regionSize     = CY_PROT_SIZE_256B,           /* ignored for master; must be valid */
        .subregions     = 0U,
        .userPermission = CY_PROT_PERM_RW,
        .privPermission = CY_PROT_PERM_RW,
        .secure         = false,
        .pcMatch        = false,
        .pcMask         = PCMASK_NONE,
    };
    (void)Cy_Prot_ConfigSmpuMasterStruct(PROT_SMPU_SMPU_STRUCT0, &lock);
    (void)Cy_Prot_EnableSmpuMasterStruct(PROT_SMPU_SMPU_STRUCT0);
}

/* ------------------------------------------------------------------ *
 * Boundary B — fixed PPUs over the CRYPTO block (D4).
 *
 * Six purpose-built fixed PPUs (MAIN/CRYPTO/BOOT/KEY0/KEY1/BUF). Slave att =
 * pcMask NONE: no ordinary PC is allowed, so CM4 (PC2) faults; CM0+ still drives
 * CRYPTO because PC0 keeps unrestricted access to fixed PPUs (TRM §6). Fixed PPU
 * pairs have no UX/PX (MMIO isn't executed) and the master read is const-1, so
 * the master att is likewise a write-deny lock (pcMask NONE = PC0-only manage).
 * ------------------------------------------------------------------ */
static void prot_setup_crypto_ppu(void)
{
    PERI_MS_PPU_FX_Type *const crypto_ppus[] = {
        PERI_MS_PPU_FX_CRYPTO_MAIN,
        PERI_MS_PPU_FX_CRYPTO_CRYPTO,
        PERI_MS_PPU_FX_CRYPTO_BOOT,
        PERI_MS_PPU_FX_CRYPTO_KEY0,
        PERI_MS_PPU_FX_CRYPTO_KEY1,
        PERI_MS_PPU_FX_CRYPTO_BUF,
    };

    for (uint32_t i = 0U; i < (sizeof crypto_ppus / sizeof crypto_ppus[0]); ++i)
    {
        (void)Cy_Prot_ConfigPpuFixedSlaveAtt(crypto_ppus[i], PCMASK_NONE,
                                             CY_PROT_PERM_RW, CY_PROT_PERM_RW, false);
        (void)Cy_Prot_ConfigPpuFixedMasterAtt(crypto_ppus[i], PCMASK_NONE,
                                             CY_PROT_PERM_RW, CY_PROT_PERM_RW, false);
    }
}

/* ------------------------------------------------------------------ *
 * PC assignment (D2, revised) — only CM4 is moved; CM0+ is left in its boot PC0.
 * ------------------------------------------------------------------ */
static void prot_assign_contexts(void)
{
    /* CM4 (still held in reset — this runs before Cy_SysEnableCM4): allow only
     * PC2, and mark it NON-secure. Two consequences:
     *   - Door 1 (TRM clamp): a CM4 write of MPU_MS_CTL.PC to 0 or 1 (not in its
     *     mask) is a silent no-op — CM4 stays in PC2. Combined with PC0/PC1 being
     *     CM0+-only special contexts, CM4 has no path to the bypass.
     *   - Door 2 (TRM: PC_MASK register is "controlled by the secure CPU"): with
     *     CM4 non-secure and CM0+ the secure CPU, CM4 cannot rewrite its own
     *     SMPU_MS[14]_CTL to re-add PC0. */
    (void)Cy_Prot_ConfigBusMaster(CPUSS_MS_ID_CM4, true, false,
                                  PCMASK_FOR_PC(PC_CM4_UNTRUSTED));   /* 0x0002 = PC2 only */
    (void)Cy_Prot_SetActivePC(CPUSS_MS_ID_CM4, PC_CM4_UNTRUSTED);

    /* CM0+ is intentionally NOT reconfigured: it remains in PC0 as the secure
     * manager. Cy_Prot_ConfigBusMaster writes the whole MSx_CTL and would clear
     * PC0's mask bit, risking the secure CPU's own bypass — so we leave it.
     * TRM-VERIFY: that CM0+ is already the secure master by default in NORMAL
     * lifecycle (so Door 2 holds). If not, establish CM0+ as secure WITHOUT
     * disturbing its PC0 permission. */
}

void cm0p_prot_install_walls(void)
{
    /* Regions + locks first (while CM0+ is unrestricted PC0 and can write them),
     * then move CM4 to PC2. CM4 is in reset throughout (precedes Cy_SysEnableCM4,
     * ADR-0020 D2), so there is no window where its PC is live but a wall is down. */
    prot_setup_key_smpu();
    prot_setup_crypto_ppu();
    prot_assign_contexts();
}

#else  /* !FBL_M4_SEAM5_PROT */

/* Walls disabled at this build: no-op so main_cm0p.c can call unconditionally
 * and the Seam-4 image is unchanged. */
void cm0p_prot_install_walls(void)
{
}

#endif /* FBL_M4_SEAM5_PROT */
