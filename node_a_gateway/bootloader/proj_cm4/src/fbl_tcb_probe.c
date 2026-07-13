/*
 * fbl_tcb_probe.c — CM4 TCB-isolation fault demo (M4 Seam 5, ADR-0020 D6).
 *
 * SCAFFOLD, debug-build-only (FBL_M4_SEAM5_PROBE). See fbl_tcb_probe.h for the
 * intent. This is the *negative control's* counterpart: prot_config_cm0p.c puts
 * the walls up on the CM0+; this pokes them from the CM4 and shows the fault.
 *
 * Observability: unlike the CM0+ debug structs (which sit at FIXED shared-SRAM
 * addresses because the CM4's OpenOCD can't see CM0+ symbols), this runs on the
 * CM4 we debug directly — so a plain file-scope record read by symbol is enough.
 * Breakpoint fbl_tcb_probe / HardFault_Handler, or inspect g_tcb_probe.
 */
#include "fbl_tcb_probe.h"

#ifdef FBL_M4_SEAM5_PROBE

#include "cy_pdl.h"   /* SCB, CRYPTO_BASE, fixed-width types via CMSIS */
#include <stdint.h>

/* Representative addresses of the two walled resources (ADR-0020 D3/D4):
 *   - The public key lives somewhere in the CM0+ image flash 0x1000_0000..
 *     0x1002_0000; the SMPU denies the WHOLE 128 KB to CM4, so reading the base
 *     is as good a probe as the key's exact address (which CM4 doesn't know).
 *   - The CRYPTO block MMIO base; the fixed PPUs deny it to CM4. */
#define PROBE_KEY_ADDR      0x10000000UL
#define PROBE_CRYPTO_ADDR   ((uint32_t)CRYPTO_BASE)   /* 0x4010_0000 */

/* Progress / capture record. `stage` shows how far the probe got before a fault;
 * the *_val fields hold what was read when the wall was DOWN. On a fault the
 * handler fills fault_* (BFAR = the faulting address confirms which wall bit). */
typedef struct
{
    volatile uint32_t stage;        /* 1=start 2=key read done 3=crypto read done 9=finished clean */
    volatile uint32_t key_val;      /* byte read from PROBE_KEY_ADDR (walls down) */
    volatile uint32_t crypto_val;   /* word read from PROBE_CRYPTO_ADDR (walls down) */
    volatile uint32_t faulted;      /* 0xFA0170ED once HardFault_Handler ran */
    volatile uint32_t hfsr;         /* SCB->HFSR */
    volatile uint32_t cfsr;         /* SCB->CFSR (BusFault status in bits [15:8]) */
    volatile uint32_t bfar;         /* SCB->BFAR — faulting data address, if valid */
} tcb_probe_dbg_t;

volatile tcb_probe_dbg_t g_tcb_probe;

/* SCB->CFSR BFARVALID (BusFault Address Register valid). */
#define CFSR_BFARVALID   (1UL << 15)

/* Catch the wall's refusal. By default BusFault is disabled, so a walled access
 * escalates to HardFault — we override that (weak symbol in the MTB startup).
 * Capture the fault status then trap; we do NOT try to recover (PC-advance) — the
 * before/after proof is two builds (walls off vs on), not one run. Reading SCB is
 * safe here; storing to the volatile record makes the state visible over OpenOCD. */
void HardFault_Handler(void)
{
    g_tcb_probe.hfsr = SCB->HFSR;
    g_tcb_probe.cfsr = SCB->CFSR;
    g_tcb_probe.bfar = ((SCB->CFSR & CFSR_BFARVALID) != 0UL) ? SCB->BFAR : 0xFFFFFFFFUL;
    g_tcb_probe.faulted = 0xFA0170EDUL;   /* "FAULTED" */
    for (;;)
    {
        /* trapped — inspect g_tcb_probe over OpenOCD */
    }
}

void fbl_tcb_probe(void)
{
    g_tcb_probe.stage = 1U;

    /* Boundary A (SMPU): read a byte of the CM0+ image / key region. A READ is
     * used deliberately — the TRM notes a PPU/SMPU-violating WRITE can be buffered
     * by the AHB bridge and OK'd back to the master (CPUSS_BUFF_CTL.WRITE_BUFF),
     * so only a read reliably surfaces the bus error on the faulting master. */
    const volatile uint8_t *key = (const volatile uint8_t *)PROBE_KEY_ADDR;
    g_tcb_probe.key_val = (uint32_t)(*key);   /* faults here when walls are UP */
    g_tcb_probe.stage = 2U;

    /* Boundary B (PPU): read a CRYPTO register. (If the walls are up, control
     * never reaches here — the key read above already trapped. To exercise the
     * PPU in isolation, temporarily skip the key read.) */
    const volatile uint32_t *crypto = (const volatile uint32_t *)PROBE_CRYPTO_ADDR;
    g_tcb_probe.crypto_val = *crypto;
    g_tcb_probe.stage = 3U;

    /* Reached only with the walls DOWN: both accesses succeeded. */
    g_tcb_probe.stage = 9U;
}

#else  /* !FBL_M4_SEAM5_PROBE */

void fbl_tcb_probe(void)
{
}

#endif /* FBL_M4_SEAM5_PROBE */
