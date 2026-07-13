/*
 * test_prot_region.c — Unity tests for the SMPU/PPU region-descriptor math
 * (M4 Seam 5, ADR-0020 D6).
 *
 * The enforcement is bench-only (no host model of the bus fabric); this suite
 * pins the one thing that CAN be wrong on the host: the CAT1A region geometry —
 * power-of-two size encoding, natural alignment pulling the region base below
 * the target, and the 8-way subregion-disable mask. The headline case is the
 * real one: the whole 128 KB CM0+ image (0x1000_0000, the public-key region,
 * ADR-0020 D3) must encode to an exact 128 KB region with an empty mask.
 *
 * Fails against the stub prot_region.c (returns INVALID / false) until the
 * implementation lands. Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "prot_region.h"

/* Flash-map constants under test (fbl_cm4.ld): the CM0+ image region. */
#define CM0P_IMAGE_BASE   0x10000000UL
#define CM0P_IMAGE_SIZE   0x00020000UL   /* FLASH_CM0P_SIZE = 128 KB */

void setUp(void) {}
void tearDown(void) {}

/* ---- size encoding: size == 1u << (code + 1) ---- */
void test_encode_size_powers_of_two(void)
{
    TEST_ASSERT_EQUAL_UINT8(7U,  prot_encode_region_size(256U));      /* 2^8  */
    TEST_ASSERT_EQUAL_UINT8(8U,  prot_encode_region_size(512U));      /* 2^9  */
    TEST_ASSERT_EQUAL_UINT8(15U, prot_encode_region_size(0x10000U));  /* 64 KB */
    TEST_ASSERT_EQUAL_UINT8(16U, prot_encode_region_size(0x20000U));  /* 128 KB */
    TEST_ASSERT_EQUAL_UINT8(19U, prot_encode_region_size(0x100000U)); /* 1 MB  */
}

void test_encode_size_rejects_non_power_of_two_and_too_small(void)
{
    TEST_ASSERT_EQUAL_UINT8(PROT_SIZE_INVALID, prot_encode_region_size(0U));
    TEST_ASSERT_EQUAL_UINT8(PROT_SIZE_INVALID, prot_encode_region_size(128U));    /* < 256 */
    TEST_ASSERT_EQUAL_UINT8(PROT_SIZE_INVALID, prot_encode_region_size(255U));
    TEST_ASSERT_EQUAL_UINT8(PROT_SIZE_INVALID, prot_encode_region_size(300U));    /* not 2^n */
    TEST_ASSERT_EQUAL_UINT8(PROT_SIZE_INVALID, prot_encode_region_size(0x18000U));/* 96 KB */
}

/* ---- the real region: whole 128 KB CM0+ image, exact, no mask ---- */
void test_cover_cm0p_image_is_exact_128k(void)
{
    prot_region_t r;
    TEST_ASSERT_TRUE(prot_region_cover(CM0P_IMAGE_BASE, CM0P_IMAGE_SIZE, &r));
    TEST_ASSERT_EQUAL_HEX32(CM0P_IMAGE_BASE, r.base);
    TEST_ASSERT_EQUAL_UINT8(16U, r.size_code);        /* 128 KB */
    TEST_ASSERT_EQUAL_UINT8(0U, r.subregion_disable); /* whole region active */
    TEST_ASSERT_TRUE(r.exact);
}

void test_cover_aligned_64k(void)
{
    prot_region_t r;
    TEST_ASSERT_TRUE(prot_region_cover(0x10010000UL, 0x10000UL, &r));
    TEST_ASSERT_EQUAL_HEX32(0x10010000UL, r.base);
    TEST_ASSERT_EQUAL_UINT8(15U, r.size_code);
    TEST_ASSERT_EQUAL_UINT8(0U, r.subregion_disable);
    TEST_ASSERT_TRUE(r.exact);
}

/* ---- subregion mask: 96 KB target inside a 128 KB region disables the top two
 * 16 KB subregions (bits 6,7), still exact (0x18000 == 6 * 16 KB). ---- */
void test_cover_96k_disables_top_two_subregions(void)
{
    prot_region_t r;
    TEST_ASSERT_TRUE(prot_region_cover(0x10000000UL, 0x18000UL, &r));
    TEST_ASSERT_EQUAL_HEX32(0x10000000UL, r.base);
    TEST_ASSERT_EQUAL_UINT8(16U, r.size_code);        /* enclosing 128 KB */
    TEST_ASSERT_EQUAL_UINT8(0xC0U, r.subregion_disable);
    TEST_ASSERT_TRUE(r.exact);
}

/* ---- natural alignment pulls the region base BELOW `base`: a 96 KB target at
 * 0x..08000 needs a 128 KB region based at 0x..00000, disabling the low two
 * 16 KB subregions (bits 0,1). ---- */
void test_cover_alignment_pulls_base_down(void)
{
    prot_region_t r;
    TEST_ASSERT_TRUE(prot_region_cover(0x10008000UL, 0x18000UL, &r));
    TEST_ASSERT_EQUAL_HEX32(0x10000000UL, r.base);
    TEST_ASSERT_EQUAL_UINT8(16U, r.size_code);
    TEST_ASSERT_EQUAL_UINT8(0x03U, r.subregion_disable);
    TEST_ASSERT_TRUE(r.exact);
}

/* ---- non-subregion-aligned length => enabled subregions spill past the target,
 * so exact is false (the caller is told the cover is loose). 5.25 KB in an 8 KB
 * region (1 KB subregions): subregions 0..5 active cover 6 KB, mask disables 6,7. ---- */
void test_cover_reports_inexact_when_length_unaligned(void)
{
    prot_region_t r;
    TEST_ASSERT_TRUE(prot_region_cover(0x10000000UL, 0x1500UL, &r));
    TEST_ASSERT_EQUAL_UINT8(12U, r.size_code);        /* 8 KB */
    TEST_ASSERT_EQUAL_UINT8(0xC0U, r.subregion_disable);
    TEST_ASSERT_FALSE(r.exact);
}

/* ---- defensive: empty length and overflow rejected ---- */
void test_cover_rejects_bad_args(void)
{
    prot_region_t r;
    TEST_ASSERT_FALSE(prot_region_cover(0x10000000UL, 0U, &r));
    TEST_ASSERT_FALSE(prot_region_cover(0xFFFF0000UL, 0x20000UL, &r)); /* base+len overflows */
    TEST_ASSERT_FALSE(prot_region_cover(0x10000000UL, 0x1000UL, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_size_powers_of_two);
    RUN_TEST(test_encode_size_rejects_non_power_of_two_and_too_small);
    RUN_TEST(test_cover_cm0p_image_is_exact_128k);
    RUN_TEST(test_cover_aligned_64k);
    RUN_TEST(test_cover_96k_disables_top_two_subregions);
    RUN_TEST(test_cover_alignment_pulls_base_down);
    RUN_TEST(test_cover_reports_inexact_when_length_unaligned);
    RUN_TEST(test_cover_rejects_bad_args);
    return UNITY_END();
}
