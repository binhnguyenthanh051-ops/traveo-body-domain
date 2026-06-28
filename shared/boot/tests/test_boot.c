/*
 * test_boot.c — Unity tests for the FBL boot core.
 *
 * Covers: reset-cause classification (ADR-0007 D2/D3), the .noinit handshake,
 * the BREG boot-loop counter rule (D4), image digest + header + vector validity
 * (ADR-0008 D3), the knock dwell (D2), and the boot-decision tree (D1).
 *
 * Fails against the current stub boot.c (step 5 implements). Test harness —
 * exempt from MISRA (see docs/coding-standard.md).
 */
#include "unity.h"
#include "boot.h"
#include "fbl_port.h"
#include <string.h>

/* ---- fake-port API (boot_port_fake.c) ---- */
extern void              fake_reset(void);
extern void              fake_set_cause(fbl_reset_cause_t c);
extern void              fake_set_lifecycle(fbl_lifecycle_t lc);
extern fbl_handshake_t  *fake_noinit(void);
extern void              fake_set_backup(uint8_t i, uint32_t v);
extern uint32_t          fake_get_backup(uint8_t i);
extern void              fake_set_contact_at(uint32_t ms);
extern void              fake_set_app(const uint8_t *b, uint32_t n);
extern int               fake_prime_called(void);
extern int               fake_jump_called(void);
extern int               fake_programming_called(void);

/* ---- independent reference CRC32 (IEEE), to build/expect images ---- */
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

/* ---- representative valid vectors ---- */
#define IMG_MSP    0x08020000u   /* top of SRAM, 8-byte aligned */
#define IMG_RESET  0x10040101u   /* in app flash (FBL_APP_FLASH_BASE), Thumb bit set */
#define IMG_BODY   16u
#define IMG_LEN    (FBL_APP_HEADER_OFFSET + (uint32_t)sizeof(fbl_app_header_t) + IMG_BODY)
#define IMG_BUFSZ  (IMG_LEN + FBL_DIGEST_SIZE)

static uint8_t g_img[IMG_BUFSZ];

/* Build a valid app image into g_img; flags inject specific corruptions. */
static uint32_t build_image(int bad_magic, int corrupt_body, int corrupt_trailer)
{
    memset(g_img, 0, sizeof g_img);

    uint32_t msp = IMG_MSP, reset = IMG_RESET;
    memcpy(g_img + 0, &msp, 4);
    memcpy(g_img + 4, &reset, 4);

    fbl_app_header_t h;
    memset(&h, 0, sizeof h);
    h.magic       = bad_magic ? 0xDEADBEEFu : FBL_APP_HEADER_MAGIC;
    h.hdr_version = 1u;
    h.hdr_size    = (uint16_t)sizeof h;
    h.image_len   = IMG_LEN;
    memcpy(g_img + FBL_APP_HEADER_OFFSET, &h, sizeof h);

    for (uint32_t i = 0; i < IMG_BODY; ++i) {
        g_img[FBL_APP_HEADER_OFFSET + sizeof h + i] = (uint8_t)(0xA0u + i);
    }

    /* Trailer is computed over the CLEAN body; the body is corrupted only
     * afterwards, so a corrupted image genuinely mismatches its stored digest
     * (computing the CRC after corruption would just validate the corruption). */
    uint32_t crc = ref_crc32(g_img, IMG_LEN);
    if (corrupt_trailer) { crc ^= 0xFFFFFFFFu; }
    memcpy(g_img + IMG_LEN, &crc, FBL_DIGEST_SIZE);

    if (corrupt_body) {
        g_img[FBL_APP_HEADER_OFFSET + sizeof h] ^= 0xFFu;
    }
    return IMG_LEN;
}

void setUp(void)    { fake_reset(); }
void tearDown(void) {}

/* ==================================================================
 * Reset-cause classification (ADR-0007 D2/D3)
 * ================================================================ */
void test_classify_power_on_primes(void)   { TEST_ASSERT_EQUAL(FBL_NOINIT_PRIME, fbl_classify_noinit(FBL_RST_POWER_ON)); }
void test_classify_hibernate_primes(void)  { TEST_ASSERT_EQUAL(FBL_NOINIT_PRIME, fbl_classify_noinit(FBL_RST_HIBERNATE)); }
void test_classify_other_primes(void)      { TEST_ASSERT_EQUAL(FBL_NOINIT_PRIME, fbl_classify_noinit(FBL_RST_OTHER)); }
void test_classify_software_reads(void)    { TEST_ASSERT_EQUAL(FBL_NOINIT_READ,  fbl_classify_noinit(FBL_RST_SOFTWARE)); }
void test_classify_watchdog_reads(void)    { TEST_ASSERT_EQUAL(FBL_NOINIT_READ,  fbl_classify_noinit(FBL_RST_WATCHDOG)); }
void test_classify_debug_reads(void)       { TEST_ASSERT_EQUAL(FBL_NOINIT_READ,  fbl_classify_noinit(FBL_RST_DEBUG)); }

/* ==================================================================
 * .noinit handshake (ADR-0007 D1)
 * ================================================================ */
void test_handshake_init_default_is_valid_boot_app(void)
{
    fbl_handshake_t h;
    memset(&h, 0xFF, sizeof h);
    fbl_handshake_init_default(&h);
    TEST_ASSERT_EQUAL_HEX32(FBL_HANDSHAKE_MAGIC, h.magic);
    TEST_ASSERT_EQUAL_UINT8(FBL_BOOT_APP, h.mode);
    TEST_ASSERT_TRUE(fbl_handshake_valid(&h));
}

void test_handshake_rejects_bad_magic(void)
{
    fbl_handshake_t h;
    fbl_handshake_init_default(&h);
    h.magic ^= 0x1u;
    TEST_ASSERT_FALSE(fbl_handshake_valid(&h));
}

void test_handshake_rejects_bad_crc(void)
{
    fbl_handshake_t h;
    fbl_handshake_init_default(&h);
    h.crc32 ^= 0x1u;
    TEST_ASSERT_FALSE(fbl_handshake_valid(&h));
}

void test_handshake_crc_changes_with_mode(void)
{
    fbl_handshake_t h;
    fbl_handshake_init_default(&h);
    uint32_t c0 = fbl_handshake_crc(&h);
    h.mode = FBL_PROGRAMMING_REQUESTED;
    TEST_ASSERT_NOT_EQUAL(c0, fbl_handshake_crc(&h));
}

/* ==================================================================
 * BREG boot-loop counter rule (ADR-0007 D4)
 * ================================================================ */
void test_counter_power_on_clears(void)        { TEST_ASSERT_EQUAL_UINT32(0u, fbl_counter_update(FBL_RST_POWER_ON, false, 5u)); }
void test_counter_sw_with_prog_clears(void)    { TEST_ASSERT_EQUAL_UINT32(0u, fbl_counter_update(FBL_RST_SOFTWARE, true, 5u)); }
void test_counter_sw_without_prog_increments(void){ TEST_ASSERT_EQUAL_UINT32(6u, fbl_counter_update(FBL_RST_SOFTWARE, false, 5u)); }
void test_counter_watchdog_increments(void)    { TEST_ASSERT_EQUAL_UINT32(6u, fbl_counter_update(FBL_RST_WATCHDOG, false, 5u)); }
void test_counter_debug_unchanged(void)        { TEST_ASSERT_EQUAL_UINT32(5u, fbl_counter_update(FBL_RST_DEBUG, false, 5u)); }
void test_counter_hibernate_unchanged(void)    { TEST_ASSERT_EQUAL_UINT32(5u, fbl_counter_update(FBL_RST_HIBERNATE, false, 5u)); }

/* ==================================================================
 * Image digest (ADR-0008 D3)
 * ================================================================ */
void test_digest_matches_known_crc32(void)
{
    const uint8_t msg[] = { '1','2','3','4','5','6','7','8','9' };
    uint8_t out[FBL_DIGEST_SIZE];
    TEST_ASSERT_EQUAL_size_t(FBL_DIGEST_SIZE, fbl_digest(msg, sizeof msg, out));
    uint32_t got;
    memcpy(&got, out, 4);                    /* little-endian digest */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, got);          /* CRC32("123456789") */
    TEST_ASSERT_EQUAL_HEX32(ref_crc32(msg, sizeof msg), got);
}

/* ==================================================================
 * Vector sanity (ADR-0008 D3 / B8)
 * ================================================================ */
void test_vectors_valid_pass(void)            { TEST_ASSERT_TRUE(fbl_vector_sane(IMG_MSP, IMG_RESET)); }
void test_vectors_msp_outside_sram_fail(void) { TEST_ASSERT_FALSE(fbl_vector_sane(0x20000000u, IMG_RESET)); }
void test_vectors_msp_misaligned_fail(void)   { TEST_ASSERT_FALSE(fbl_vector_sane(0x08000004u, IMG_RESET)); }
void test_vectors_reset_outside_flash_fail(void){ TEST_ASSERT_FALSE(fbl_vector_sane(IMG_MSP, 0x20000001u)); }
void test_vectors_reset_no_thumb_fail(void)   { TEST_ASSERT_FALSE(fbl_vector_sane(IMG_MSP, 0x10040100u)); }

/* ==================================================================
 * App image validity (ADR-0008 D3)
 * ================================================================ */
void test_image_valid_passes(void)
{
    build_image(0, 0, 0);
    TEST_ASSERT_TRUE(fbl_app_image_valid(g_img, IMG_BUFSZ));
}
void test_image_bad_header_magic_fails(void)
{
    build_image(1, 0, 0);
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, IMG_BUFSZ));
}
void test_image_corrupt_body_fails(void)
{
    build_image(0, 1, 0);
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, IMG_BUFSZ));
}
void test_image_corrupt_trailer_fails(void)
{
    build_image(0, 0, 1);
    TEST_ASSERT_FALSE(fbl_app_image_valid(g_img, IMG_BUFSZ));
}

/* ==================================================================
 * Knock dwell (ADR-0008 D2)
 * ================================================================ */
void test_dwell_returns_true_on_contact(void)
{
    fake_set_contact_at(200u);
    TEST_ASSERT_TRUE(fbl_dwell_for_tool(FBL_KNOCK_WINDOW_MS));
}
void test_dwell_returns_false_on_timeout(void)
{
    fake_set_contact_at(0xFFFFFFFFu);            /* never */
    TEST_ASSERT_FALSE(fbl_dwell_for_tool(FBL_KNOCK_WINDOW_MS));
}

/* ==================================================================
 * Boot-decision tree (ADR-0008 D1) — via the fake port
 * ================================================================ */
static void set_valid_app(void)        { build_image(0, 0, 0); fake_set_app(g_img, IMG_BUFSZ); }
static void set_valid_breg(uint32_t counter)
{
    fake_set_backup(FBL_BREG_MAGIC_IDX, FBL_BACKUP_MAGIC);
    fake_set_backup(FBL_BREG_COUNTER_IDX, counter);
}

void test_decide_cold_normal_valid_app_no_knock_jumps(void)
{
    fake_set_cause(FBL_RST_POWER_ON);
    fake_set_lifecycle(FBL_LC_NORMAL);
    set_valid_breg(0u);
    set_valid_app();
    fake_set_contact_at(0xFFFFFFFFu);            /* nobody knocks */
    TEST_ASSERT_EQUAL(FBL_ACTION_JUMP_APP, fbl_run_boot());
}

void test_decide_secure_ignores_knock_and_jumps(void)
{
    fake_set_cause(FBL_RST_POWER_ON);
    fake_set_lifecycle(FBL_LC_SECURE);
    set_valid_breg(0u);
    set_valid_app();
    fake_set_contact_at(0u);                     /* knock immediately — must be ignored in SECURE */
    TEST_ASSERT_EQUAL(FBL_ACTION_JUMP_APP, fbl_run_boot());
}

void test_decide_cold_normal_knock_enters_programming(void)
{
    fake_set_cause(FBL_RST_POWER_ON);
    fake_set_lifecycle(FBL_LC_NORMAL);
    set_valid_breg(0u);
    set_valid_app();
    fake_set_contact_at(0u);                     /* knock within the window */
    TEST_ASSERT_EQUAL(FBL_ACTION_PROGRAMMING_MODE, fbl_run_boot());
}

void test_decide_programming_request_enters_programming(void)
{
    fake_set_cause(FBL_RST_SOFTWARE);            /* warm: .noinit is read */
    set_valid_breg(0u);
    set_valid_app();
    fbl_handshake_t *h = fake_noinit();
    fbl_handshake_init_default(h);
    h->mode  = FBL_PROGRAMMING_REQUESTED;
    h->crc32 = fbl_handshake_crc(h);
    TEST_ASSERT_EQUAL(FBL_ACTION_PROGRAMMING_MODE, fbl_run_boot());
}

void test_decide_bootloop_counter_over_threshold_enters_programming(void)
{
    fake_set_cause(FBL_RST_SOFTWARE);
    set_valid_breg(FBL_BOOTLOOP_THRESHOLD);      /* +1 on this sw reset trips it */
    set_valid_app();
    fbl_handshake_t *h = fake_noinit();
    fbl_handshake_init_default(h);               /* boot-app, no programming request */
    TEST_ASSERT_EQUAL(FBL_ACTION_PROGRAMMING_MODE, fbl_run_boot());
}

void test_decide_invalid_app_fails_safe_to_programming(void)
{
    fake_set_cause(FBL_RST_POWER_ON);
    fake_set_lifecycle(FBL_LC_NORMAL);
    set_valid_breg(0u);
    build_image(0, 1, 0);                        /* corrupt app */
    fake_set_app(g_img, IMG_BUFSZ);
    fake_set_contact_at(0xFFFFFFFFu);
    TEST_ASSERT_EQUAL(FBL_ACTION_PROGRAMMING_MODE, fbl_run_boot());
}

void test_decide_hibernate_valid_app_jumps_without_knock(void)
{
    fake_set_cause(FBL_RST_HIBERNATE);
    fake_set_lifecycle(FBL_LC_NORMAL);
    set_valid_breg(0u);
    set_valid_app();
    fake_set_contact_at(0u);                     /* a knock must NOT hold us — hibernate skips the window */
    TEST_ASSERT_EQUAL(FBL_ACTION_JUMP_APP, fbl_run_boot());
}

/* ==================================================================
 * M2-5 — App-side reprogram vs the boot-loop fallback (ADR-0007 D7/D9)
 *
 * The App writes a programming-request with boot_handshake_encode (the write
 * mirror), then a software reset. The FBL must enter programming mode AND clear
 * the counter every time, so a legitimate reflash never looks like a crash-loop.
 * ================================================================ */

void test_handshake_encode_programming_request_is_valid(void)
{
    fbl_handshake_t h;
    boot_handshake_encode(&h, FBL_PROGRAMMING_REQUESTED);
    TEST_ASSERT_TRUE(fbl_handshake_valid(&h));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FBL_PROGRAMMING_REQUESTED, h.mode);

    boot_handshake_encode(&h, FBL_BOOT_APP);
    TEST_ASSERT_TRUE(fbl_handshake_valid(&h));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FBL_BOOT_APP, h.mode);
}

void test_m2_5_repeated_reprogram_never_trips_bootloop(void)
{
    const uint32_t iterations = FBL_BOOTLOOP_THRESHOLD + 5u;

    /* Start with the counter sitting right at the trip threshold: a legitimate
     * reprogram must still clear it, not push it over. */
    set_valid_breg(FBL_BOOTLOOP_THRESHOLD);
    set_valid_app();

    for (uint32_t i = 0u; i < iterations; ++i)
    {
        /* App writes the request into .noinit, then triggers a software reset. */
        boot_handshake_encode(fake_noinit(), FBL_PROGRAMMING_REQUESTED);
        fake_set_cause(FBL_RST_SOFTWARE);

        /* FBL: programming mode every time, and the counter is cleared (D4), so
         * the boot-loop fallback (counter > N) never trips. */
        TEST_ASSERT_EQUAL(FBL_ACTION_PROGRAMMING_MODE, fbl_run_boot());
        TEST_ASSERT_EQUAL_UINT32(0u, fake_get_backup(FBL_BREG_COUNTER_IDX));
    }
}

/* ==================================================================
 * Runner
 * ================================================================ */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_classify_power_on_primes);
    RUN_TEST(test_classify_hibernate_primes);
    RUN_TEST(test_classify_other_primes);
    RUN_TEST(test_classify_software_reads);
    RUN_TEST(test_classify_watchdog_reads);
    RUN_TEST(test_classify_debug_reads);

    RUN_TEST(test_handshake_init_default_is_valid_boot_app);
    RUN_TEST(test_handshake_rejects_bad_magic);
    RUN_TEST(test_handshake_rejects_bad_crc);
    RUN_TEST(test_handshake_crc_changes_with_mode);

    RUN_TEST(test_counter_power_on_clears);
    RUN_TEST(test_counter_sw_with_prog_clears);
    RUN_TEST(test_counter_sw_without_prog_increments);
    RUN_TEST(test_counter_watchdog_increments);
    RUN_TEST(test_counter_debug_unchanged);
    RUN_TEST(test_counter_hibernate_unchanged);

    RUN_TEST(test_digest_matches_known_crc32);

    RUN_TEST(test_vectors_valid_pass);
    RUN_TEST(test_vectors_msp_outside_sram_fail);
    RUN_TEST(test_vectors_msp_misaligned_fail);
    RUN_TEST(test_vectors_reset_outside_flash_fail);
    RUN_TEST(test_vectors_reset_no_thumb_fail);

    RUN_TEST(test_image_valid_passes);
    RUN_TEST(test_image_bad_header_magic_fails);
    RUN_TEST(test_image_corrupt_body_fails);
    RUN_TEST(test_image_corrupt_trailer_fails);

    RUN_TEST(test_dwell_returns_true_on_contact);
    RUN_TEST(test_dwell_returns_false_on_timeout);

    RUN_TEST(test_decide_cold_normal_valid_app_no_knock_jumps);
    RUN_TEST(test_decide_secure_ignores_knock_and_jumps);
    RUN_TEST(test_decide_cold_normal_knock_enters_programming);
    RUN_TEST(test_decide_programming_request_enters_programming);
    RUN_TEST(test_decide_bootloop_counter_over_threshold_enters_programming);
    RUN_TEST(test_decide_invalid_app_fails_safe_to_programming);
    RUN_TEST(test_decide_hibernate_valid_app_jumps_without_knock);

    RUN_TEST(test_handshake_encode_programming_request_is_valid);
    RUN_TEST(test_m2_5_repeated_reprogram_never_trips_bootloop);

    return UNITY_END();
}
