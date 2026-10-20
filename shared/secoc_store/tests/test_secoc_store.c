/*
 * test_secoc_store.c — the two-slot work-flash freshness store (ADR-0024)
 * against the fake work flash, plus the real sender boot path
 * (secoc_tx_boot) on top of it for the REQ-SECOC-006 anti-reuse property.
 *
 * tearDown checks two invariants after every test: the store never read a
 * slot with a CPU read (a torn word would fault the CPU on target, D3), and it
 * never reprogrammed a word without erasing it first. Exempt from MISRA.
 */
#include "unity.h"
#include "secoc_store.h"
#include "secoc_freshness.h"
#include "wflash_fake.h"

#define SLOT0   (WF_BASE + (1U * WF_SECTOR_SIZE))
#define SLOT1   (WF_BASE + (3U * WF_SECTOR_SIZE))
#define DOM_TX  0U
#define DOM_RX  1U
#define STEPS_PER_SAVE  6       /* 1 erase + 4 body words + 1 mark */

static const secoc_store_cfg_t g_cfg = { &g_wflash_fake, { SLOT0, SLOT1 } };

static void reboot(void)
{
    wflash_fake_power_cycle();
}

static uint16_t load_ok(uint8_t domain)
{
    uint16_t e = 0xEEEEU;
    TEST_ASSERT_TRUE(g_secoc_store.load(domain, &e));
    return e;
}

void setUp(void)
{
    wflash_fake_reset();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, wflash_fake_cpu_reads(), "store used a CPU read");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, wflash_fake_reprogram_violations(),
                                   "store reprogrammed a word without erasing");
}

/* ---- mount ---------------------------------------------------------------- */

/* @test ADR-0024 D3 : both slots blank = factory state, every domain loads cold */
void test_factory_flash_mounts_cold(void)
{
    TEST_ASSERT_EQUAL(SECOC_STORE_FACTORY, secoc_store_mount(&g_cfg));
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store.load(DOM_TX, &e));
    TEST_ASSERT_FALSE(g_secoc_store.load(DOM_RX, &e));
}

/* @test REQ-SECOC-006 : commit is durable -- the value survives a power cycle */
/* @test ADR-0024 D2 */
void test_commit_survives_power_cycle(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 41U));

    reboot();
    TEST_ASSERT_EQUAL(SECOC_STORE_OK, secoc_store_mount(&g_cfg));
    TEST_ASSERT_EQUAL_UINT16(41U, load_ok(DOM_TX));
}

/* @test ADR-0024 D1 : one record carries both domains; a domain is cold until written */
void test_domains_are_independent_and_both_persist(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 7U));
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store.load(DOM_RX, &e));     /* still cold */
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_RX, 300U));

    reboot();
    TEST_ASSERT_EQUAL(SECOC_STORE_OK, secoc_store_mount(&g_cfg));
    TEST_ASSERT_EQUAL_UINT16(7U, load_ok(DOM_TX));
    TEST_ASSERT_EQUAL_UINT16(300U, load_ok(DOM_RX));
}

/* @test ADR-0024 D2 : saves alternate slots, so the newest record is never the one erased */
void test_saves_alternate_slots_and_newest_wins(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 1U));     /* slot 0 */
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 2U));     /* slot 1 */
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 3U));     /* slot 0 again */
    TEST_ASSERT_EQUAL_UINT(2U, wflash_fake_erase_count(SLOT0));
    TEST_ASSERT_EQUAL_UINT(1U, wflash_fake_erase_count(SLOT1));

    reboot();
    TEST_ASSERT_EQUAL(SECOC_STORE_OK, secoc_store_mount(&g_cfg));
    TEST_ASSERT_EQUAL_UINT16(3U, load_ok(DOM_TX));          /* higher seq wins */
}

/* ---- power loss ------------------------------------------------------------ */

static void power_loss_at_every_step(wf_torn_mode_t mode)
{
    for (int k = 0; k <= STEPS_PER_SAVE; ++k) {
        wflash_fake_reset();
        wflash_fake_set_torn_mode(mode);
        (void)secoc_store_mount(&g_cfg);
        TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 5U));
        TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_RX, 9U));  /* both slots now hold records */

        wflash_fake_fail_after(k);
        bool saved = g_secoc_store.commit(DOM_TX, 6U);

        reboot();
        TEST_ASSERT_EQUAL_MESSAGE(SECOC_STORE_OK, secoc_store_mount(&g_cfg),
                                  "a save interrupted at any step must leave a valid record");
        TEST_ASSERT_EQUAL(k >= STEPS_PER_SAVE, saved);
        /* Never lost, never invented: the old value unless the save returned true. */
        TEST_ASSERT_EQUAL_UINT16(saved ? 6U : 5U, load_ok(DOM_TX));
        TEST_ASSERT_EQUAL_UINT16(9U, load_ok(DOM_RX));
    }
}

/* @test REQ-SECOC-006 : power loss at every erase/program step; torn words fail the ECC read */
/* @test ADR-0024 D2 */
void test_power_loss_at_every_step_torn_ecc(void)
{
    power_loss_at_every_step(WF_TORN_ECC);
}

/* @test REQ-SECOC-006 : same, torn words read back as wrong bits (mark/CRC must catch it) */
/* @test ADR-0024 D3 */
void test_power_loss_at_every_step_torn_garbage(void)
{
    power_loss_at_every_step(WF_TORN_GARBAGE);
}

/* @test REQ-SECOC-006 : a failed save changes nothing, in RAM or in flash */
void test_failed_save_keeps_the_old_value(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 20U));
    wflash_fake_fail_after(2);
    TEST_ASSERT_FALSE(g_secoc_store.commit(DOM_TX, 21U));
    TEST_ASSERT_EQUAL_UINT16(20U, load_ok(DOM_TX));         /* same session */

    reboot();
    TEST_ASSERT_EQUAL(SECOC_STORE_OK, secoc_store_mount(&g_cfg));
    TEST_ASSERT_EQUAL_UINT16(20U, load_ok(DOM_TX));         /* next boot */
}

/* @test REQ-SECOC-006 : over many boots with brown-outs, the real sender never reuses an epoch */
void test_sender_never_reuses_an_epoch_across_brownouts(void)
{
    const uint16_t ids[1] = { 0x120U };
    secoc_tx_ctx_t tx;
    uint16_t last_spent = 0U;
    unsigned booted = 0U;

    for (unsigned boot = 0U; boot < 60U; ++boot) {
        reboot();
        /* Boot 0 is clean; then a brown-out at a varying step, or none (k > steps). */
        TEST_ASSERT_NOT_EQUAL(SECOC_STORE_BROKEN, secoc_store_mount(&g_cfg));
        if (boot > 0U) {
            wflash_fake_fail_after((int)((boot * 5U) % 9U));
        }
        if (secoc_tx_boot(&tx, &g_secoc_store, DOM_TX, ids, 1U)) {
            TEST_ASSERT_TRUE_MESSAGE(tx.epoch > last_spent, "epoch reused or went backwards");
            last_spent = tx.epoch;
            booted++;
        }
    }
    TEST_ASSERT_TRUE(booted > 10U);     /* the loop really exercised successful boots */
}

/* ---- fail closed ---------------------------------------------------------- */

/* @test REQ-SECOC-006 : no valid record and not blank => BROKEN, never "cold" (no epoch restart) */
/* @test ADR-0024 D3 */
void test_both_slots_unreadable_is_broken_and_fails_closed(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 50U));
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 51U));
    wflash_fake_decay_word(SLOT0 + 4U);
    wflash_fake_decay_word(SLOT1 + 4U);

    reboot();
    TEST_ASSERT_EQUAL(SECOC_STORE_BROKEN, secoc_store_mount(&g_cfg));
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store.load(DOM_TX, &e));
    TEST_ASSERT_FALSE(g_secoc_store.commit(DOM_TX, 52U));
    const uint16_t ids[1] = { 0x120U };
    secoc_tx_ctx_t tx;
    TEST_ASSERT_FALSE(secoc_tx_boot(&tx, &g_secoc_store, DOM_TX, ids, 1U));
}

/* @test ADR-0024 D3 : a brown-out during the very FIRST save, at every step.
 * Body steps leave the mark blank => the slot is empty => still FACTORY (nothing was
 * spent). An interrupted erase or mark program leaves a non-blank torn slot next to an
 * empty one => BROKEN: indistinguishable from "the only record decayed", so it fails
 * closed, and recovery is erasing both slots. Pinned so a change is a decision, not drift. */
void test_first_save_torn_from_factory(void)
{
    for (int k = 0; k <= STEPS_PER_SAVE; ++k) {
        wflash_fake_reset();
        (void)secoc_store_mount(&g_cfg);
        wflash_fake_fail_after(k);
        bool saved = g_secoc_store.commit(DOM_TX, 1U);

        reboot();
        secoc_store_status_t st = secoc_store_mount(&g_cfg);
        if (saved) {
            TEST_ASSERT_EQUAL(SECOC_STORE_OK, st);
        } else if ((k == 0) || (k == STEPS_PER_SAVE - 1)) {
            TEST_ASSERT_EQUAL_MESSAGE(SECOC_STORE_BROKEN, st, "erase or mark torn");
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(SECOC_STORE_FACTORY, st, "body torn, mark blank");
        }
    }
}

/* @test ADR-0024 D3 : the newest record decaying falls back to the older one (residual risk) */
void test_decayed_newest_record_falls_back_to_older(void)
{
    for (int mode = 0; mode <= 1; ++mode) {
        wflash_fake_reset();
        wflash_fake_set_torn_mode((wf_torn_mode_t)mode);
        (void)secoc_store_mount(&g_cfg);
        TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 70U));    /* slot 0 */
        TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 71U));    /* slot 1, newest */
        wflash_fake_decay_word(SLOT1 + 8U);                     /* a body word */

        reboot();
        TEST_ASSERT_EQUAL(SECOC_STORE_OK, secoc_store_mount(&g_cfg));
        TEST_ASSERT_EQUAL_UINT16(70U, load_ok(DOM_TX));
    }
}

/* ---- config and arguments ------------------------------------------------- */

/* @test ADR-0024 D4 : a flash instance without blank_check/read_checked can't back the store */
void test_bad_config_is_broken(void)
{
    hal_flash_if_t no_blank = g_wflash_fake;
    no_blank.blank_check = NULL;
    hal_flash_if_t no_checked = g_wflash_fake;
    no_checked.read_checked = NULL;
    const secoc_store_cfg_t bad[] = {
        { NULL, { SLOT0, SLOT1 } },
        { &no_blank, { SLOT0, SLOT1 } },
        { &no_checked, { SLOT0, SLOT1 } },
        { &g_wflash_fake, { SLOT0, SLOT0 } },           /* both slots the same sector */
        { &g_wflash_fake, { SLOT0 + 4U, SLOT1 } },      /* not sector-aligned */
    };

    TEST_ASSERT_EQUAL(SECOC_STORE_BROKEN, secoc_store_mount(NULL));
    for (unsigned i = 0U; i < sizeof bad / sizeof bad[0]; ++i) {
        TEST_ASSERT_EQUAL(SECOC_STORE_BROKEN, secoc_store_mount(&bad[i]));
        TEST_ASSERT_FALSE(g_secoc_store.commit(DOM_TX, 1U));
    }
}

void test_bad_arguments_are_rejected(void)
{
    (void)secoc_store_mount(&g_cfg);
    TEST_ASSERT_FALSE(g_secoc_store.commit(SECOC_STORE_DOMAINS, 1U));
    TEST_ASSERT_TRUE(g_secoc_store.commit(DOM_TX, 1U));
    TEST_ASSERT_FALSE(g_secoc_store.load(DOM_TX, NULL));
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store.load(SECOC_STORE_DOMAINS, &e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_factory_flash_mounts_cold);
    RUN_TEST(test_commit_survives_power_cycle);
    RUN_TEST(test_domains_are_independent_and_both_persist);
    RUN_TEST(test_saves_alternate_slots_and_newest_wins);
    RUN_TEST(test_power_loss_at_every_step_torn_ecc);
    RUN_TEST(test_power_loss_at_every_step_torn_garbage);
    RUN_TEST(test_failed_save_keeps_the_old_value);
    RUN_TEST(test_sender_never_reuses_an_epoch_across_brownouts);
    RUN_TEST(test_both_slots_unreadable_is_broken_and_fails_closed);
    RUN_TEST(test_first_save_torn_from_factory);
    RUN_TEST(test_decayed_newest_record_falls_back_to_older);
    RUN_TEST(test_bad_config_is_broken);
    RUN_TEST(test_bad_arguments_are_rejected);
    return UNITY_END();
}
