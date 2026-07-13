/*
 * port_noinit.c — .noinit handshake region accessor + ECC priming (target).
 *
 * The accessor is board-independent: the handshake lives in the `.noinit`
 * section the linker pins at the top of SRAM (fbl.ld) and startup must not zero.
 * ECC priming is board-gated (TODO).
 */
#include "fbl_port.h"

/* In its OWN section (.fbl_handshake), pinned by fbl_cm4.ld at the shared address
 * (noinit.ld) — separate from the general .noinit the BSP/libraries use, so the
 * App and FBL agree on a fixed address regardless of how much .noinit each pulls
 * in. Never zeroed by startup (ADR-0007 D1/D8). */
static fbl_handshake_t g_handshake __attribute__((section(".fbl_handshake")));

fbl_handshake_t *fbl_port_noinit(void)
{
    return &g_handshake;
}

void fbl_port_prime_noinit(void)
{
    /*
     * ADR-0007 D3: on a prime-cause the region's ECC is invalid, so it must be
     * WRITTEN before any read. The write below establishes valid ECC; the core
     * then initialises the struct. Confirm this is a full ECC-word-granular
     * write on this part (verify in TRM) — a partial write may not validate ECC.
     * TODO(M1, board): verify ECC granularity / that this triggers an ECC write.
     */
    volatile uint32_t *p = (volatile uint32_t *)&g_handshake;
    for (uint32_t i = 0U; i < (sizeof g_handshake / sizeof(uint32_t)); ++i)
    {
        p[i] = 0U;
    }
}
