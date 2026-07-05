/*
 * test_uds_routine_control.c — Unity tests for 0x31 RoutineControl: erase
 * (ADR-0012 D6) and check-programmed-image (ADR-0012 D8, reusing
 * shared/boot's fbl_app_image_valid()/fbl_digest() unchanged from ADR-0008
 * D3 -- no new verify interface).
 *
 * Fails against the current stub uds_routine_control.c (step 5 implements).
 * Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "uds_routine_control.h"
#include "boot.h"
#include "boot_types.h"
#include <string.h>

extern void fake_flash_reset(void);
extern const uint8_t *fake_flash_buffer(void);
extern uint8_t *fake_flash_buffer_mut(void);
extern const hal_flash_if_t *fake_flash_hal(void);

static uint32_t ref_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = (uint32_t)(-(int)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

#define IMG_MSP    0x08020000u
#define IMG_RESET  0x10040101u
#define IMG_BODY   16u
#define IMG_LEN    (FBL_APP_HEADER_OFFSET + (uint32_t)sizeof(fbl_app_header_t) + IMG_BODY)
#define IMG_BUFSZ  (IMG_LEN + FBL_DIGEST_SIZE)

/* Builds a (possibly corrupted) valid-shaped app image directly into the fake
 * flash at offset 0 -- mirrors shared/boot/tests/test_boot.c's build_image,
 * duplicated here deliberately (test isolation; this module doesn't depend
 * on shared/boot's test harness). */
static void build_image(uint8_t *out, int corrupt_body)
{
    memset(out, 0, IMG_BUFSZ);

    uint32_t msp = IMG_MSP, reset = IMG_RESET;
    memcpy(out + 0, &msp, 4);
    memcpy(out + 4, &reset, 4);

    fbl_app_header_t h;
    memset(&h, 0, sizeof h);
    h.magic       = FBL_APP_HEADER_MAGIC;
    h.hdr_version = 1u;
    h.hdr_size    = (uint16_t)sizeof h;
    h.image_len   = IMG_LEN;
    memcpy(out + FBL_APP_HEADER_OFFSET, &h, sizeof h);

    for (uint32_t i = 0; i < IMG_BODY; ++i) {
        out[FBL_APP_HEADER_OFFSET + sizeof h + i] = (uint8_t)(0xA0u + i);
    }

    uint32_t crc = ref_crc32(out, IMG_LEN);
    memcpy(out + IMG_LEN, &crc, FBL_DIGEST_SIZE);

    if (corrupt_body) {
        out[FBL_APP_HEADER_OFFSET + sizeof h] ^= 0xFFu;
    }
}

void setUp(void)
{
    fake_flash_reset();
    uds_routine_control_init(fake_flash_hal(), fake_flash_buffer(), IMG_BUFSZ);
}
void tearDown(void) {}

static uds_result_t call_erase(uint32_t addr, uint32_t len, uint8_t *resp, size_t *resp_len)
{
    uint8_t req[11];
    req[0] = 0x01U;   /* routineControlType: start */
    req[1] = (uint8_t)(UDS_ROUTINE_ERASE_MEMORY >> 8);
    req[2] = (uint8_t)(UDS_ROUTINE_ERASE_MEMORY);
    req[3] = (uint8_t)(addr >> 24); req[4] = (uint8_t)(addr >> 16);
    req[5] = (uint8_t)(addr >> 8);  req[6] = (uint8_t)(addr);
    req[7] = (uint8_t)(len >> 24);  req[8] = (uint8_t)(len >> 16);
    req[9] = (uint8_t)(len >> 8);   req[10] = (uint8_t)(len);   /* 32-bit length */
    return uds_routine_control_handler()->handle(req, sizeof req, resp, 16U, resp_len);
}

static uds_result_t call_check(uint8_t *resp, size_t *resp_len)
{
    uint8_t req[3];
    req[0] = 0x01U;
    req[1] = (uint8_t)(UDS_ROUTINE_CHECK_PROGRAMMED_IMAGE >> 8);
    req[2] = (uint8_t)(UDS_ROUTINE_CHECK_PROGRAMMED_IMAGE);
    return uds_routine_control_handler()->handle(req, sizeof req, resp, 16U, resp_len);
}

void test_erase_clears_the_requested_range(void)
{
    memset(fake_flash_buffer_mut(), 0x55, 1024U);   /* pre-fill with non-erased pattern */

    uint8_t resp[16]; size_t resp_len = 0U;
    uds_result_t r = call_erase(0U, 1024U, resp, &resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
    uint8_t erased[1024]; memset(erased, 0xFF, sizeof erased);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(erased, fake_flash_buffer(), sizeof erased);
}

void test_check_programmed_image_passes_on_valid_image(void)
{
    build_image(fake_flash_buffer_mut(), 0);

    uint8_t resp[16]; size_t resp_len = 0U;
    uds_result_t r = call_check(resp, &resp_len);

    TEST_ASSERT_EQUAL(UDS_NRC_NONE, r);
}

void test_check_programmed_image_fails_on_corrupt_image(void)
{
    build_image(fake_flash_buffer_mut(), 1);

    uint8_t resp[16]; size_t resp_len = 0U;
    uds_result_t r = call_check(resp, &resp_len);

    TEST_ASSERT_NOT_EQUAL(UDS_NRC_NONE, r);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_erase_clears_the_requested_range);
    RUN_TEST(test_check_programmed_image_passes_on_valid_image);
    RUN_TEST(test_check_programmed_image_fails_on_corrupt_image);

    return UNITY_END();
}
