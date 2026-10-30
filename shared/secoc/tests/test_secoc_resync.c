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

/* F-014: the same deadlock in the OTHER direction. Node A is also a receiver
 * (of telemetry 0x200). Before the fix, an A reboot on its own raised A's floor
 * above B's telemetry epoch and nothing brought B up: 460 rejects in a row on
 * the 2026-10-24 bench. Now A sends its own authenticated sync on 0x1F0 at boot.
 * Both nodes' contexts run here, each on its own store domain. */
#define A_TX_DOM   2U        /* A's sender epoch: commands + 0x1F0 */
#define A_RX_DOM   3U        /* A's view of B's epoch */
#define B_TX_DOM   4U        /* B's sender epoch: telemetry + 0x2F0 */
#define B_RX_DOM   5U        /* B's view of A's epoch */
#define ID_TELEM   0x200U
#define ID_GW_SYNC 0x1F0U

static const uint16_t g_a_tx_ids[] = { ID_DOOR, ID_GW_SYNC };
static const uint16_t g_b_tx_ids[] = { ID_TELEM, 0x2F0U };
static const uint16_t g_a_rx_ids[] = { ID_TELEM, 0x2F0U };
static const uint16_t g_b_rx_ids[] = { ID_DOOR, ID_GW_SYNC };

/* B receives one frame from A: check, then accept (the MAC is modelled as passed). */
static secoc_fresh_verdict_t b_receives(secoc_rx_ctx_t *b_rx, uint16_t id, uint16_t e, uint16_t c)
{
    secoc_fresh_verdict_t v = secoc_rx_check(b_rx, id, e, c);
    if (v == SECOC_FRESH_OK)
    {
        TEST_ASSERT_TRUE(secoc_rx_accept(b_rx, id, e, c));
    }
    return v;
}

/* @test REQ-SECOC-008 : a Node A reboot deadlocks telemetry; A's 0x1F0 sync resyncs B */
void test_gateway_reboot_deadlocks_telemetry_then_resyncs(void)
{
    secoc_tx_ctx_t a_tx, b_tx;
    secoc_rx_ctx_t a_rx, b_rx;
    (void)secoc_tx_boot(&a_tx, &g_secoc_store_fake, A_TX_DOM, g_a_tx_ids, 2U);
    (void)secoc_rx_boot(&a_rx, &g_secoc_store_fake, A_RX_DOM, g_a_rx_ids, 2U);
    (void)secoc_tx_boot(&b_tx, &g_secoc_store_fake, B_TX_DOM, g_b_tx_ids, 2U);
    (void)secoc_rx_boot(&b_rx, &g_secoc_store_fake, B_RX_DOM, g_b_rx_ids, 2U);

    /* Telemetry B -> A at B's epoch 1 is accepted; A persists epoch 1 for B. */
    uint16_t e = 0U, c = 0U;
    (void)secoc_tx_next(&b_tx, ID_TELEM, &e, &c);
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&a_rx, ID_TELEM, e, c));
    TEST_ASSERT_TRUE(secoc_rx_accept(&a_rx, ID_TELEM, e, c));

    /* A reboots on its own. Its telemetry floor rises to 2; B is still at 1. */
    secoc_store_fake_power_cycle();
    secoc_tx_ctx_t a_tx2;
    secoc_rx_ctx_t a_rx2;
    (void)secoc_tx_boot(&a_tx2, &g_secoc_store_fake, A_TX_DOM, g_a_tx_ids, 2U);
    (void)secoc_rx_boot(&a_rx2, &g_secoc_store_fake, A_RX_DOM, g_a_rx_ids, 2U);
    TEST_ASSERT_EQUAL_UINT16(2U, a_rx2.floor);
    (void)secoc_tx_next(&b_tx, ID_TELEM, &e, &c);                    /* (1, 2) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&a_rx2, ID_TELEM, e, c));  /* deadlock */

    /* A's boot sync on 0x1F0, at A's new sender epoch, carrying its floor (2). */
    TEST_ASSERT_TRUE(secoc_tx_next(&a_tx2, ID_GW_SYNC, &e, &c));
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, b_receives(&b_rx, ID_GW_SYNC, e, c));
    TEST_ASSERT_TRUE(secoc_tx_adopt_floor(&b_tx, a_rx2.floor));      /* B: epoch 1 -> 2 */

    /* Telemetry at B's new epoch is accepted again. */
    TEST_ASSERT_TRUE(secoc_tx_next(&b_tx, ID_TELEM, &e, &c));        /* (2, 1) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, secoc_rx_check(&a_rx2, ID_TELEM, e, c));
    TEST_ASSERT_TRUE(secoc_rx_accept(&a_rx2, ID_TELEM, e, c));

    /* And old telemetry from B's epoch 1 replays into A as stale (REQ-SECOC-007). */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&a_rx2, ID_TELEM, 1U, 3U));
}

/* The sync is a protected frame under A's freshness, so a captured one is
 * refused once A has booted again: B already holds A's newer epoch. An attacker
 * can't use an old sync to move B's epoch, forwards or back.
 *
 * @test REQ-SECOC-008 : a replayed 0x1F0 from an earlier A boot is refused */
void test_replayed_gateway_sync_is_refused(void)
{
    secoc_tx_ctx_t a_tx;
    secoc_rx_ctx_t b_rx;
    (void)secoc_tx_boot(&a_tx, &g_secoc_store_fake, A_TX_DOM, g_a_tx_ids, 2U);
    (void)secoc_rx_boot(&b_rx, &g_secoc_store_fake, B_RX_DOM, g_b_rx_ids, 2U);

    /* A's first-boot sync, captured by the attacker. */
    uint16_t old_e = 0U, old_c = 0U;
    (void)secoc_tx_next(&a_tx, ID_GW_SYNC, &old_e, &old_c);          /* (1, 1) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, b_receives(&b_rx, ID_GW_SYNC, old_e, old_c));

    /* A reboots and sends its new sync; B adopts A's epoch 2. */
    secoc_store_fake_power_cycle();
    secoc_tx_ctx_t a_tx2;
    (void)secoc_tx_boot(&a_tx2, &g_secoc_store_fake, A_TX_DOM, g_a_tx_ids, 2U);
    uint16_t e = 0U, c = 0U;
    (void)secoc_tx_next(&a_tx2, ID_GW_SYNC, &e, &c);                 /* (2, 1) */
    TEST_ASSERT_EQUAL(SECOC_FRESH_OK, b_receives(&b_rx, ID_GW_SYNC, e, c));

    /* The captured first-boot sync is now stale. */
    TEST_ASSERT_EQUAL(SECOC_FRESH_STALE, secoc_rx_check(&b_rx, ID_GW_SYNC, old_e, old_c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_adopt_floor_only_moves_forward);
    RUN_TEST(test_adopt_floor_resets_counters);
    RUN_TEST(test_receiver_reboot_deadlocks_then_resyncs);
    RUN_TEST(test_gateway_reboot_deadlocks_telemetry_then_resyncs);
    RUN_TEST(test_replayed_gateway_sync_is_refused);
    return UNITY_END();
}
