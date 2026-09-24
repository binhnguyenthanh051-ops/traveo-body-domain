/*
 * test_secoc_resync.c — receiver-reset resync (ADR-0021 D5 / REQ-SECOC-008).
 * Red against the stub secoc_freshness.c.
 *
 * The scenario is the whole reason FRESHNESS_SYNC exists: a receiver reboot
 * raises its floor above the still-running sender's epoch (deadlock), and the
 * authenticated sync pushes the sender's epoch up to the floor to recover —
 * after which the old-epoch replay is closed. Modelled at the freshness layer
 * (the MAC/framing of 0x2F0 is exercised in test_secoc). Exempt from MISRA.
 */
#include "unity.h"
#include "secoc_freshness.h"
#include "secoc_freshness_store_fake.h"

#define A_DOM    0U          /* sender A's own epoch */
#define B_DOM    1U          /* receiver B's accepted-epoch high-water */
#define ID_DOOR  0x120U

static const uint16_t g_ids[] = { ID_DOOR };

void setUp(void)    { secoc_store_fake_reset(); }
void tearDown(void) {}

/* @test ADR-0021 D5 */
void test_adopt_floor_only_moves_forward(void)
{
    secoc_tx_ctx_t a;
    (void)secoc_tx_boot(&a, &g_secoc_store_fake, A_DOM, g_ids, 1U);  /* epoch 1 */
    TEST_ASSERT_FALSE(secoc_tx_adopt_floor(&a, 1U));                 /* not newer ⇒ no move */
    TEST_ASSERT_EQUAL_UINT16(1U, a.epoch);
    TEST_ASSERT_TRUE(secoc_tx_adopt_floor(&a, 5U));                  /* forward ⇒ move */
    TEST_ASSERT_EQUAL_UINT16(5U, a.epoch);
}

/* @test ADR-0021 D5 */
void test_adopt_floor_resets_counters(void)
{
    secoc_tx_ctx_t a;
    (void)secoc_tx_boot(&a, &g_secoc_store_fake, A_DOM, g_ids, 1U);
    uint16_t e = 0U, c = 0U;
    (void)secoc_tx_next(&a, ID_DOOR, &e, &c);   /* (1,1) */
    (void)secoc_tx_next(&a, ID_DOOR, &e, &c);   /* (1,2) */
    TEST_ASSERT_TRUE(secoc_tx_adopt_floor(&a, 4U));
    TEST_ASSERT_TRUE(secoc_tx_next(&a, ID_DOOR, &e, &c));
    TEST_ASSERT_EQUAL_UINT16(4U, e);
    TEST_ASSERT_EQUAL_UINT16(1U, c);            /* counter restarted in the new epoch */
}

/* @test REQ-SECOC-008 */
void test_receiver_reboot_deadlocks_then_resyncs(void)
{
    /* A (sender) and B (receiver) both cold-boot. */
    secoc_tx_ctx_t a;
    secoc_rx_ctx_t b;
    (void)secoc_tx_boot(&a, &g_secoc_store_fake, A_DOM, g_ids, 1U);  /* A epoch 1 */
    (void)secoc_rx_boot(&b, &g_secoc_store_fake, B_DOM, g_ids, 1U);  /* B floor 1, current 0 */

    /* Normal traffic: A→B (1,1) accepted; B persists epoch 1. */
    uint16_t e = 0U, c = 0U;
    (void)secoc_tx_next(&a, ID_DOOR, &e, &c);
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&b, ID_DOOR, e, c));
    secoc_rx_accept(&b, ID_DOOR, e, c);

    /* B reboots (its accepted-epoch high-water survives). New floor = 2. */
    secoc_store_fake_power_cycle();
    secoc_rx_ctx_t b2;
    (void)secoc_rx_boot(&b2, &g_secoc_store_fake, B_DOM, g_ids, 1U);
    TEST_ASSERT_EQUAL_UINT16(2U, b2.floor);

    /* A is still at epoch 1 → its next frame is now BELOW the floor: deadlock. */
    (void)secoc_tx_next(&a, ID_DOOR, &e, &c);                       /* (1, 2) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&b2, ID_DOOR, e, c));

    /* Resync: B authenticates FRESHNESS_SYNC carrying its floor (2); A adopts. */
    TEST_ASSERT_TRUE(secoc_tx_adopt_floor(&a, b2.floor));
    TEST_ASSERT_EQUAL_UINT16(2U, a.epoch);

    /* A→B at the new epoch is accepted again. */
    TEST_ASSERT_TRUE(secoc_tx_next(&a, ID_DOOR, &e, &c));           /* (2, 1) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&b2, ID_DOOR, e, c));
    secoc_rx_accept(&b2, ID_DOOR, e, c);

    /* And the pre-reboot replay is now closed: an old epoch-1 frame is stale. */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&b2, ID_DOOR, 1U, 2U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_adopt_floor_only_moves_forward);
    RUN_TEST(test_adopt_floor_resets_counters);
    RUN_TEST(test_receiver_reboot_deadlocks_then_resyncs);
    return UNITY_END();
}
