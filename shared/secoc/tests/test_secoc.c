/*
 * test_secoc.c — secured-frame layer (ADR-0021 D1/D2/D9 / REQ-SECOC-001..004,
 * 013). Red against the stub secoc.c.
 *
 * The load-bearing properties: a round trip recovers the PDU + freshness; any
 * tamper (MAC or PDU) fails closed; a frame replayed on a DIFFERENT CAN ID
 * fails because the Data ID is bound into the MAC (the cross-ID substitution
 * fix); and an oracle failure is fail-safe, never a silent pass. Uses the
 * deterministic (non-crypto) MAC fake. Exempt from MISRA.
 */
#include "unity.h"
#include "secoc.h"
#include "secoc_mac_fake.h"
#include <string.h>

#define ID_DOOR   0x120U
#define ID_LIGHT  0x121U
#define ID_TELEM  0x200U

static const secoc_crypto_t g_cy  = { &g_secoc_mac_fake, SECOC_FAKE_KEY_ID };
static const secoc_crypto_t g_cy0 = { &g_secoc_mac_fake, 0U };   /* oracle refuses key 0 */

void setUp(void)    {}
void tearDown(void) {}

/* @test ADR-0021 D1 */
void test_ct_equal(void)
{
    const uint8_t x[4] = { 1U, 2U, 3U, 4U };
    const uint8_t y[4] = { 1U, 2U, 3U, 4U };
    const uint8_t z[4] = { 1U, 2U, 3U, 5U };
    TEST_ASSERT_TRUE(secoc_ct_equal(x, y, 4U));
    TEST_ASSERT_FALSE(secoc_ct_equal(x, z, 4U));
}

/* @test REQ-SECOC-003 */
void test_secure_length_is_pdu_plus_trailer(void)
{
    const uint8_t pdu[1] = { 0xA5U };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame);
    TEST_ASSERT_EQUAL_size_t(sizeof pdu + SECOC_TRAILER_LEN, n);
}

/* @test REQ-SECOC-001 */
/* @test REQ-SECOC-002 */
/* @test REQ-SECOC-003 */
void test_round_trip_recovers_pdu_and_freshness(void)
{
    const uint8_t pdu[3] = { 0xDEU, 0xADU, 0xBEU };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_DOOR, 7U, 42U, pdu, sizeof pdu, frame, sizeof frame);
    TEST_ASSERT_GREATER_THAN_size_t(0U, n);

    uint16_t e = 0U, c = 0U;
    uint8_t out[8]; size_t out_len = 0U;
    secoc_verify_result_t r = secoc_verify(&g_cy, ID_DOOR, frame, n,
                                           &e, &c, out, sizeof out, &out_len);
    TEST_ASSERT_EQUAL(SECOC_OK, r);
    TEST_ASSERT_EQUAL_UINT16(7U, e);
    TEST_ASSERT_EQUAL_UINT16(42U, c);
    TEST_ASSERT_EQUAL_size_t(sizeof pdu, out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pdu, out, sizeof pdu);
}

/* @test REQ-SECOC-001 */
void test_tampered_mac_is_rejected(void)
{
    const uint8_t pdu[2] = { 0x11U, 0x22U };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame);
    frame[n - 1U] ^= 0xFFU;                     /* flip a MAC byte */

    uint16_t e, c; uint8_t out[8]; size_t out_len;
    TEST_ASSERT_EQUAL(SECOC_BAD_MAC,
        secoc_verify(&g_cy, ID_DOOR, frame, n, &e, &c, out, sizeof out, &out_len));
}

/* @test REQ-SECOC-001 */
void test_tampered_pdu_is_rejected(void)
{
    const uint8_t pdu[2] = { 0x11U, 0x22U };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame);
    frame[0] ^= 0xFFU;                          /* flip a payload byte */

    uint16_t e, c; uint8_t out[8]; size_t out_len;
    TEST_ASSERT_EQUAL(SECOC_BAD_MAC,
        secoc_verify(&g_cy, ID_DOOR, frame, n, &e, &c, out, sizeof out, &out_len));
}

/* REQ-SECOC-004: the Data ID is bound into the MAC, so the same bytes replayed
 * on a different CAN ID fail — this is the cross-ID substitution fix. */
/* @test REQ-SECOC-004 */
void test_replay_on_different_id_is_rejected(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame);

    uint16_t e, c; uint8_t out[8]; size_t out_len;
    /* verify as if received on ID_LIGHT: Data ID differs ⇒ MAC mismatch */
    TEST_ASSERT_EQUAL(SECOC_BAD_MAC,
        secoc_verify(&g_cy, ID_LIGHT, frame, n, &e, &c, out, sizeof out, &out_len));
}

/* @test ADR-0021 D9 */
/* @test REQ-SECOC-010 */
void test_oracle_failure_is_fail_safe(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    /* secure with a working oracle... */
    size_t n = secoc_secure(&g_cy, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame);
    TEST_ASSERT_GREATER_THAN_size_t(0U, n);

    /* ...but verify through an oracle that can't produce a tag ⇒ MAC_ERROR, not OK. */
    uint16_t e, c; uint8_t out[8]; size_t out_len;
    TEST_ASSERT_EQUAL(SECOC_MAC_ERROR,
        secoc_verify(&g_cy0, ID_DOOR, frame, n, &e, &c, out, sizeof out, &out_len));

    /* And secure with a failing oracle produces nothing. */
    TEST_ASSERT_EQUAL_size_t(0U,
        secoc_secure(&g_cy0, ID_DOOR, 1U, 1U, pdu, sizeof pdu, frame, sizeof frame));
}

/* @test REQ-SECOC-003 */
void test_short_frame_is_bad_length(void)
{
    uint8_t frame[8] = { 0U };                  /* < trailer (12) — can't hold freshness+MAC */
    uint16_t e, c; uint8_t out[8]; size_t out_len;
    TEST_ASSERT_EQUAL(SECOC_BAD_LENGTH,
        secoc_verify(&g_cy, ID_DOOR, frame, sizeof frame, &e, &c, out, sizeof out, &out_len));
}

/* REQ-SECOC-013: telemetry rides the identical secured path. */
/* @test REQ-SECOC-013 */
void test_telemetry_round_trip(void)
{
    const uint8_t rpt[4] = { 0x01U, 0x02U, 0x03U, 0x04U };
    uint8_t frame[32];
    size_t n = secoc_secure(&g_cy, ID_TELEM, 3U, 9U, rpt, sizeof rpt, frame, sizeof frame);

    uint16_t e, c; uint8_t out[8]; size_t out_len;
    TEST_ASSERT_EQUAL(SECOC_OK,
        secoc_verify(&g_cy, ID_TELEM, frame, n, &e, &c, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpt, out, sizeof rpt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ct_equal);
    RUN_TEST(test_secure_length_is_pdu_plus_trailer);
    RUN_TEST(test_round_trip_recovers_pdu_and_freshness);
    RUN_TEST(test_tampered_mac_is_rejected);
    RUN_TEST(test_tampered_pdu_is_rejected);
    RUN_TEST(test_replay_on_different_id_is_rejected);
    RUN_TEST(test_oracle_failure_is_fail_safe);
    RUN_TEST(test_short_frame_is_bad_length);
    RUN_TEST(test_telemetry_round_trip);
    return UNITY_END();
}
