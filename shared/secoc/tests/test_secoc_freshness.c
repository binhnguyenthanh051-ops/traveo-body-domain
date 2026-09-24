/*
 * test_secoc_freshness.c — sender generation + receiver accept rule (ADR-0021
 * D3/D4 / REQ-SECOC-005..007, 009). Red against the stub secoc_freshness.c.
 *
 * Backed by the store fake so the persistence contract (commit-before-spend,
 * one-write-per-boot, per-boot increment) is exercised end to end. Exempt from
 * MISRA.
 */
#include "unity.h"
#include "secoc_freshness.h"
#include "secoc_freshness_store_fake.h"

#define TX_DOM   0U
#define RX_DOM   1U
#define ID_DOOR  0x120U
#define ID_LIGHT 0x121U

static const uint16_t g_ids[] = { ID_DOOR, ID_LIGHT };

void setUp(void)    { secoc_store_fake_reset(); }
void tearDown(void) {}

/* ---------- sender ---------- */

/* @test REQ-SECOC-006 */
void test_tx_boot_commits_epoch_before_use(void)
{
    secoc_tx_ctx_t tx;
    TEST_ASSERT_TRUE(secoc_tx_boot(&tx, &g_secoc_store_fake, TX_DOM, g_ids, 2U));
    /* cold ⇒ persisted 0 ⇒ commit(1) durable ⇒ use epoch 1 */
    TEST_ASSERT_EQUAL_UINT16(1U, tx.epoch);
    TEST_ASSERT_EQUAL_UINT(1U, secoc_store_fake_commit_count());
    uint16_t persisted = 0U;
    TEST_ASSERT_TRUE(g_secoc_store_fake.load(TX_DOM, &persisted));
    TEST_ASSERT_EQUAL_UINT16(1U, persisted);
}

/* @test REQ-SECOC-005 */
void test_tx_next_counter_is_per_id(void)
{
    secoc_tx_ctx_t tx;
    (void)secoc_tx_boot(&tx, &g_secoc_store_fake, TX_DOM, g_ids, 2U);

    uint16_t e = 0U, c = 0U;
    TEST_ASSERT_TRUE(secoc_tx_next(&tx, ID_DOOR, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(1U, e);
    TEST_ASSERT_EQUAL_UINT16(1U, c);                 /* first counter is 1 */
    TEST_ASSERT_TRUE(secoc_tx_next(&tx, ID_DOOR, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(2U, c);
    TEST_ASSERT_TRUE(secoc_tx_next(&tx, ID_LIGHT, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(1U, c);                 /* LIGHT independent of DOOR */
}

/* @test REQ-SECOC-005 */
void test_tx_next_unknown_id_fails(void)
{
    secoc_tx_ctx_t tx;
    (void)secoc_tx_boot(&tx, &g_secoc_store_fake, TX_DOM, g_ids, 2U);
    uint16_t e = 0U, c = 0U;
    TEST_ASSERT_FALSE(secoc_tx_next(&tx, 0x777U, &e, &c));
}

/* @test REQ-SECOC-009 */
void test_tx_counter_rollover_bumps_epoch(void)
{
    secoc_tx_ctx_t tx;
    (void)secoc_tx_boot(&tx, &g_secoc_store_fake, TX_DOM, g_ids, 2U);

    uint16_t e = 0U, c = 0U;
    /* advance DOOR silently to just below the wrap */
    for (uint32_t k = 1U; k <= 0xFFFEU; ++k) {
        (void)secoc_tx_next(&tx, ID_DOOR, &e, &c);
    }
    TEST_ASSERT_TRUE(secoc_tx_next(&tx, ID_DOOR, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(1U, e);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFU, c);            /* last value in epoch 1 */

    /* the next one wraps ⇒ epoch bump (durable), counter restarts at 1 */
    TEST_ASSERT_TRUE(secoc_tx_next(&tx, ID_DOOR, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(2U, e);
    TEST_ASSERT_EQUAL_UINT16(1U, c);
    TEST_ASSERT_EQUAL_UINT(2U, secoc_store_fake_commit_count());  /* boot + rollover */
}

/* @test REQ-SECOC-006 */
void test_epoch_increments_each_boot(void)
{
    secoc_tx_ctx_t tx1;
    TEST_ASSERT_TRUE(secoc_tx_boot(&tx1, &g_secoc_store_fake, TX_DOM, g_ids, 2U));
    TEST_ASSERT_EQUAL_UINT16(1U, tx1.epoch);

    secoc_store_fake_power_cycle();                  /* reboot: stored epoch survives */
    secoc_tx_ctx_t tx2;
    TEST_ASSERT_TRUE(secoc_tx_boot(&tx2, &g_secoc_store_fake, TX_DOM, g_ids, 2U));
    TEST_ASSERT_EQUAL_UINT16(2U, tx2.epoch);         /* loads 1, commits 2, uses 2 */
}

/* ---------- receiver ---------- */

/* @test REQ-SECOC-007 */
/* @test REQ-SECOC-008 */
void test_rx_boot_raises_floor_above_persisted(void)
{
    secoc_rx_ctx_t rx;
    TEST_ASSERT_TRUE(secoc_rx_boot(&rx, &g_secoc_store_fake, RX_DOM, g_ids, 2U));
    TEST_ASSERT_EQUAL_UINT16(1U, rx.floor);          /* cold ⇒ persisted 0 ⇒ floor 1 */
    TEST_ASSERT_EQUAL_UINT16(0U, rx.current);        /* current = highest accepted (none yet) */
}

/* @test REQ-SECOC-007 */
void test_rx_accepts_monotonic_rejects_replay(void)
{
    secoc_rx_ctx_t rx;
    (void)secoc_rx_boot(&rx, &g_secoc_store_fake, RX_DOM, g_ids, 2U);

    /* epoch 1 counter 1: first in-epoch frame accepted */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&rx, ID_DOOR, 1U, 1U));
    secoc_rx_accept(&rx, ID_DOOR, 1U, 1U);

    /* exact replay: same counter now ≤ high-water ⇒ stale */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&rx, ID_DOOR, 1U, 1U));

    /* gap tolerated: 1 → 5 accepted; then an older 3 rejected */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&rx, ID_DOOR, 1U, 5U));
    secoc_rx_accept(&rx, ID_DOOR, 1U, 5U);
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&rx, ID_DOOR, 1U, 3U));
}

/* @test REQ-SECOC-007 */
void test_rx_rejects_below_floor(void)
{
    secoc_rx_ctx_t rx;
    (void)secoc_rx_boot(&rx, &g_secoc_store_fake, RX_DOM, g_ids, 2U);
    /* floor is 1; epoch 0 is the entire pre-reset epoch */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&rx, ID_DOOR, 0U, 9U));
}

/* @test REQ-SECOC-007 */
void test_rx_adopts_newer_epoch_and_resets_high_water(void)
{
    secoc_rx_ctx_t rx;
    (void)secoc_rx_boot(&rx, &g_secoc_store_fake, RX_DOM, g_ids, 2U);

    secoc_rx_accept(&rx, ID_DOOR, 1U, 40U);          /* high-water[DOOR] = 40 in epoch 1 */

    /* a newer epoch is accepted even with a small counter ... */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&rx, ID_DOOR, 2U, 1U));
    secoc_rx_accept(&rx, ID_DOOR, 2U, 1U);
    TEST_ASSERT_EQUAL_UINT16(2U, rx.current);
    /* ... and LIGHT's high-water was reset by the epoch adoption */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&rx, ID_LIGHT, 2U, 1U));
}

/* @test REQ-SECOC-007 */
void test_rx_unknown_id(void)
{
    secoc_rx_ctx_t rx;
    (void)secoc_rx_boot(&rx, &g_secoc_store_fake, RX_DOM, g_ids, 2U);
    TEST_ASSERT_EQUAL(SECOC_FRESH_UNKNOWN, secoc_rx_check(&rx, 0x777U, 1U, 1U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_boot_commits_epoch_before_use);
    RUN_TEST(test_tx_next_counter_is_per_id);
    RUN_TEST(test_tx_next_unknown_id_fails);
    RUN_TEST(test_tx_counter_rollover_bumps_epoch);
    RUN_TEST(test_epoch_increments_each_boot);
    RUN_TEST(test_rx_boot_raises_floor_above_persisted);
    RUN_TEST(test_rx_accepts_monotonic_rejects_replay);
    RUN_TEST(test_rx_rejects_below_floor);
    RUN_TEST(test_rx_adopts_newer_epoch_and_resets_high_water);
    RUN_TEST(test_rx_unknown_id);
    return UNITY_END();
}
