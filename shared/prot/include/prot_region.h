/*
 * prot_region.h — protection-unit region descriptor math (M4 Seam 5, ADR-0020 D6).
 *
 * The ONE host-testable slice of the TCB-isolation work: computing a CAT1A
 * SMPU/PPU region descriptor from plain address/length constants. The bus-fabric
 * ENFORCEMENT has no host model and is proven on the bench (ADR-0020 D6); this
 * module is only the geometry — power-of-2 region size, natural alignment, and
 * the 8-way subregion-disable mask — which is exactly the fiddly, off-by-one-prone
 * bit-math that benefits from a Unity test before it ever reaches silicon.
 *
 * Hardware-independent (ADR rule 1): no vendor/ModusToolbox headers. The target
 * config (proj_cm0p) maps prot_region_t onto the PDL Cy_Prot_* structs; the
 * size_code is numerically the CAT1A REGION_SIZE field (see prot_encode_region_size).
 */
#ifndef PROT_REGION_H
#define PROT_REGION_H

#include <stdbool.h>
#include <stdint.h>

/* CAT1A SMPU/PPU minimum region is 256 B; sizes are powers of two. */
#define PROT_REGION_MIN_SIZE   256U

/* Returned by prot_encode_region_size() when the size is not a valid CAT1A
 * region size (not a power of two, or < PROT_REGION_MIN_SIZE). 0xFF is not a
 * legal 5-bit REGION_SIZE code, so it is unambiguous. */
#define PROT_SIZE_INVALID      0xFFU

/* One protection region, ready to hand to the target Cy_Prot_* config.
 *   size_code : CAT1A REGION_SIZE field — region spans (1u << (size_code + 1))
 *               bytes (so 256 B == 7, 64 KB == 15, 128 KB == 16). Numerically
 *               equal to the PDL cy_en_prot_size_t enum value.
 *   subregion_disable : bit i (0..7) disables subregion i, each (size/8) bytes,
 *               counting from `base`. A disabled subregion is NOT covered by the
 *               region's access rule. 0 = whole region active.
 *   exact : true iff the enabled subregions cover [base, base+len) with no spill
 *               over either end (i.e. len and the offset are subregion-aligned). */
typedef struct
{
    uint32_t base;
    uint8_t  size_code;
    uint8_t  subregion_disable;
    bool     exact;
} prot_region_t;

/* Encode a byte size to the CAT1A REGION_SIZE code (size == 1u << (code + 1)).
 * Returns PROT_SIZE_INVALID if `size_bytes` is not a power of two >= 256. */
uint8_t prot_encode_region_size(uint32_t size_bytes);

/* Compute the SMPU/PPU descriptor for the smallest naturally-aligned power-of-two
 * region that fully contains [base, base+len), disabling every subregion that
 * falls entirely outside that target range so the access rule covers as little
 * extra as the 1/8 granularity allows.
 *
 * Returns false (and leaves *out unspecified) if len == 0, base+len overflows
 * 32 bits, or `out` is NULL. On success `out->exact` reports whether the cover
 * is tight. For our use (the whole 128 KB CM0+ image, ADR-0020 D3) the region is
 * an exact power of two and the mask is 0. */
bool prot_region_cover(uint32_t base, uint32_t len, prot_region_t *out);

#endif /* PROT_REGION_H */
