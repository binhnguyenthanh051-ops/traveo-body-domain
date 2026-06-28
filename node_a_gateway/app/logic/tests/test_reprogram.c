/*
 * test_reprogram.c — Unity tests for the App-side programming-request (ADR-0007 D7).
 *
 * app_request_reprogram() must encode a valid PROGRAMMING_REQUESTED handshake
 * into .noinit and then trigger exactly one reset, in that order. Test harness —
 * exempt from MISRA (see docs/coding-standard.md).
 */
#include "unity.h"
#include "reprogram.h"
#include "app_port.h"
#include "boot.h"
#include "boot_types.h"

extern void              fake_app_reset(void);
extern fbl_handshake_t  *fake_app_region(void);
extern int               fake_app_reset_called(void);
extern int               fake_app_mode_at_reset(void);

void setUp(void)    { fake_app_reset(); }
void tearDown(void) {}

void test_reprogram_writes_valid_programming_request(void)
{
    app_request_reprogram();

    fbl_handshake_t *h = fake_app_region();
    TEST_ASSERT_TRUE(fbl_handshake_valid(h));                       /* magic + crc good */
    TEST_ASSERT_EQUAL_UINT8(FBL_PROGRAMMING_REQUESTED, h->mode);
}

void test_reprogram_triggers_exactly_one_reset(void)
{
    app_request_reprogram();
    TEST_ASSERT_EQUAL_INT(1, fake_app_reset_called());
}

void test_reprogram_writes_before_reset(void)
{
    app_request_reprogram();
    /* The region already held the request when the reset fired. */
    TEST_ASSERT_EQUAL_INT(FBL_PROGRAMMING_REQUESTED, fake_app_mode_at_reset());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reprogram_writes_valid_programming_request);
    RUN_TEST(test_reprogram_triggers_exactly_one_reset);
    RUN_TEST(test_reprogram_writes_before_reset);
    return UNITY_END();
}
