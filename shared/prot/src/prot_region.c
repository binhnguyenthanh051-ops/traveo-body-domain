/*
 * prot_region.c — SMPU/PPU region descriptor math (M4 Seam 5, ADR-0020 D6).
 *
 * CAT1A protection regions are power-of-two sized (>= 256 B), naturally aligned,
 * and sub-divided into 8 subregions each of which can be individually disabled.
 * This computes, from a plain {base, len}, the smallest aligned power-of-two
 * region that contains the target and the subregion-disable mask that trims the
 * access rule back toward the target at 1/8 granularity. Pure integer math, no
 * vendor headers (ADR rule 1) — the target maps size_code onto cy_en_prot_size_t.
 */
#include "prot_region.h"

uint8_t prot_encode_region_size(uint32_t size_bytes)
{
    /* Must be a power of two and at least the 256 B minimum. */
    if ((size_bytes < PROT_REGION_MIN_SIZE) ||
        ((size_bytes & (size_bytes - 1U)) != 0U))
    {
        return PROT_SIZE_INVALID;
    }

    /* size == 1u << (code + 1)  =>  code = log2(size) - 1. */
    uint8_t code = 0U;
    uint32_t s = size_bytes;
    while (s > 1U)
    {
        s >>= 1;
        ++code;
    }
    return (uint8_t)(code - 1U);
}

bool prot_region_cover(uint32_t base, uint32_t len, prot_region_t *out)
{
    if ((out == (prot_region_t *)0) || (len == 0U))
    {
        return false;
    }

    const uint64_t end = (uint64_t)base + (uint64_t)len;   /* exclusive, may be 2^32 */
    if (end > 0x100000000ULL)                              /* runs past the 32-bit space */
    {
        return false;
    }

    /* Smallest power-of-two region R (>= 256 B) whose natural alignment still
     * contains [base, end): grow until region_base(R) + R covers end. Use 64-bit
     * for R so the R == 2^32 whole-space case does not wrap. */
    uint64_t r = PROT_REGION_MIN_SIZE;
    while (r <= 0x100000000ULL)
    {
        const uint64_t region_base = (uint64_t)base & ~(r - 1U);
        if ((region_base + r) >= end)
        {
            break;
        }
        r <<= 1;
    }
    if (r > 0x100000000ULL)
    {
        return false;   /* cannot be expressed as a single region (shouldn't happen for 32-bit) */
    }

    const uint32_t region_base = (uint32_t)((uint64_t)base & ~(r - 1U));
    const uint64_t sub = r >> 3;                 /* subregion size, >= 32 B */

    /* Disable every subregion that lies entirely outside [base, end). */
    uint8_t disable = 0U;
    for (uint32_t i = 0U; i < 8U; ++i)
    {
        const uint64_t sub_start = (uint64_t)region_base + ((uint64_t)i * sub);
        const uint64_t sub_end   = sub_start + sub;
        const bool intersects = (sub_start < end) && (sub_end > (uint64_t)base);
        if (!intersects)
        {
            disable |= (uint8_t)(1U << i);
        }
    }

    /* Exact iff both target ends fall on a subregion boundary (the enabled
     * subregions then tile [base, end) with no spill). */
    const bool exact = (((uint64_t)base % sub) == 0U) && ((end % sub) == 0U);

    out->base              = region_base;
    out->size_code         = prot_encode_region_size((uint32_t)r);   /* r <= 2^31 here for 32-bit targets */
    out->subregion_disable = disable;
    out->exact             = exact;
    return true;
}
