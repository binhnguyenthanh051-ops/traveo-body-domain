/*
 * test_boot_secure.c — Unity tests for fbl_app_image_valid() in SHA-256 /
 * secure-boot mode (M4 Seam 4). Built with -DFBL_DIGEST_ALGO=FBL_DIGEST_SHA256,
 * so the digest-compare is replaced by a crypto_verify_image() delegation to
 * the M0+ (ADR-0016). Here the M0+ is the scripted fake port, so this pins the
 * FBL-side wiring: header + full 100-byte trailer bounds, vector sanity, and —
 * the load-bearing part — the verdict->bool mapping and fail-safe (ADR-0016
 * D5): only VALID leads to a valid image; INVALID and ERROR (dead/timed-out
 * M0+) both mean "not valid", never a jump.
 *
 * The fake M0+ never dereferences the image address, so the pointer->u32
 * truncation in fbl_app_image_valid is harmless here (it matters only on the
 * real 32-bit target, where the address is exact). Test harness — MISRA-exempt.
 */
#include "unity.h"
#include "boot.h"
#include "crypto_service.h"
#include "crypto_msg.h"
#include <string.h>

extern void fake_ipc_reset(void);
extern void fake_ipc_script_response(const uint8_t *resp, size_t len);
extern void fake_ipc_script_no_response(void);
extern const ipc_port_if_t *fake_ipc_port(void);

#define IMG_MSP    0x08020000u   /* top of SRAM, 8-byte aligned */
#define IMG_RESET  0x10040101u   /* in app flash (FBL_APP_FLASH_BASE), Thumb bit set */
#define IMG_LEN    0x200u

static uint8_t g_img[IMG_LEN + FBL_TRAILER_SIZE];

/* Build a structurally-valid SHA-256 image: sane vectors, good header, and a
 * 100-byte trailer (dummy hash+sig, real key_id — the fake M0+ returns a
 * scripted verdict regardless of the trailer bytes). Returns region_len. */
static uint32_t build_image(uint32_t key_id, int bad_magic)
{
    memset(g_img, 0, sizeof g_img);

    uint32_t msp = IMG_MSP;
    uint32_t reset = IMG_RESET;
    memcpy(&g_img[0], &msp, 4);
    memcpy(&g_img[4], &reset, 4);

    fbl_app_header_t h;
    h.magic = bad_magic ? 0xDEADBEEFu : FBL_APP_HEADER_MAGIC;
    h.hdr_version = 1u;
    h.hdr_size = (uint16_t)sizeof(fbl_app_header_t);
    h.image_len = IMG_LEN;
    memcpy(&g_img[FBL_APP_HEADER_OFFSET], &h, sizeof h);

    /* trailer at image_len: hash[32] + signature[64] + key_id[4] */
    memcpy(&g_img[IMG_LEN + FBL_DIGEST_SIZE + FBL_SIG_SIZE], &key_id, sizeof key_id);
    return IMG_LEN + FBL_TRAILER_SIZE;
}

/* Arm the fake M0+ to answer the next verify with this verdict. */
static void script_verdict(crypto_verdict_t v)
{
    crypto_msg_t m;
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, v, &m);
    uint8_t buf[CRYPTO_MSG_MAX_WIRE];
    size_t n = crypto_msg_encode(&m, buf, sizeof buf);
    fake_ipc_script_response(buf, n);
}

void setUp(void)
{
    fake_ipc_reset();
    crypto_service_init(fake_ipc_port());
}
void tearDown(void) {}

void test_m0plus_valid_makes_image_valid(void)
{
    uint32_t region = build_image(1u, 0);
    script_verdict(CRYPTO_VERDICT_VALID);
    TEST_ASSERT_TRUE(fbl_app_image_valid(g_img, region));
}

void test_m0plus_invalid_rejects_image(void)
{
    uint32_t region = build_image(1u, 0);
    script_verdict(CRYPTO_VERDICT_INVALID);
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, region));
}

void test_m0plus_timeout_rejects_image(void)   /* fail-safe: dead M0+ != jump */
{
    uint32_t region = build_image(1u, 0);
    fake_ipc_script_no_response();
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, region));
}

void test_bad_header_magic_rejected_before_verify(void)
{
    uint32_t region = build_image(1u, 1 /* bad magic */);
    script_verdict(CRYPTO_VERDICT_VALID);   /* even a VALID verdict must not save it */
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, region));
}

void test_region_too_small_for_trailer_rejected(void)
{
    uint32_t region = build_image(1u, 0);
    script_verdict(CRYPTO_VERDICT_VALID);
    /* one byte short of the full 100-byte trailer must fail the bounds check */
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, region - 1u));
}

void test_bad_vectors_rejected(void)
{
    uint32_t region = build_image(1u, 0);
    uint32_t bad_msp = 0x20000000u;               /* outside SRAM */
    memcpy(&g_img[0], &bad_msp, 4);
    script_verdict(CRYPTO_VERDICT_VALID);
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, region));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_m0plus_valid_makes_image_valid);
    RUN_TEST(test_m0plus_invalid_rejects_image);
    RUN_TEST(test_m0plus_timeout_rejects_image);
    RUN_TEST(test_bad_header_magic_rejected_before_verify);
    RUN_TEST(test_region_too_small_for_trailer_rejected);
    RUN_TEST(test_bad_vectors_rejected);
    return UNITY_END();
}
