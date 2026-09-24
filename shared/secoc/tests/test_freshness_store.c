/*
 * test_freshness_store.c — pins the SecOC persistence port contract (ADR-0021
 * D8 / REQ-SECOC-006) against the host fake. This suite is the reference the M6
 * eeprom_emu backing must also satisfy, so it is GREEN now (the fake is the
 * reference impl) — unlike the code-under-development suites, which are red.
 *
 * Load-bearing properties: a never-written domain loads cold (false); a commit
 * is durable (survives a power_cycle); domains are independent; commits are
 * counted so callers can assert one-write-per-boot (D3). Exempt from MISRA.
 */
#include "unity.h"
#include "secoc_freshness_store_fake.h"

#define DOM_A   0U
#define DOM_B   1U

void setUp(void)    { secoc_store_fake_reset(); }
void tearDown(void) {}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_cold_domain_loads_false(void)
{
    uint16_t e = 0xEEEEU;
    TEST_ASSERT_FALSE(g_secoc_store_fake.load(DOM_A, &e));
}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_commit_then_load_returns_value(void)
{
    TEST_ASSERT_TRUE(g_secoc_store_fake.commit(DOM_A, 7U));
    uint16_t e = 0U;
    TEST_ASSERT_TRUE(g_secoc_store_fake.load(DOM_A, &e));
    TEST_ASSERT_EQUAL_UINT16(7U, e);
}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_commit_is_counted(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, secoc_store_fake_commit_count());
    (void)g_secoc_store_fake.commit(DOM_A, 1U);
    (void)g_secoc_store_fake.commit(DOM_A, 2U);
    TEST_ASSERT_EQUAL_UINT(2U, secoc_store_fake_commit_count());
}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_domains_are_independent(void)
{
    (void)g_secoc_store_fake.commit(DOM_A, 10U);
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store_fake.load(DOM_B, &e));   /* B still cold */
    (void)g_secoc_store_fake.commit(DOM_B, 20U);
    TEST_ASSERT_TRUE(g_secoc_store_fake.load(DOM_A, &e));
    TEST_ASSERT_EQUAL_UINT16(10U, e);
    TEST_ASSERT_TRUE(g_secoc_store_fake.load(DOM_B, &e));
    TEST_ASSERT_EQUAL_UINT16(20U, e);
}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_value_survives_power_cycle(void)
{
    (void)g_secoc_store_fake.commit(DOM_A, 42U);
    secoc_store_fake_power_cycle();
    uint16_t e = 0U;
    TEST_ASSERT_TRUE(g_secoc_store_fake.load(DOM_A, &e));    /* durable across reboot */
    TEST_ASSERT_EQUAL_UINT16(42U, e);
    TEST_ASSERT_EQUAL_UINT(0U, secoc_store_fake_commit_count()); /* session tally reset */
}

/* @test ADR-0021 D8 */
/* @test REQ-SECOC-006 */
void test_reset_wipes_to_cold(void)
{
    (void)g_secoc_store_fake.commit(DOM_A, 5U);
    secoc_store_fake_reset();
    uint16_t e = 0U;
    TEST_ASSERT_FALSE(g_secoc_store_fake.load(DOM_A, &e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cold_domain_loads_false);
    RUN_TEST(test_commit_then_load_returns_value);
    RUN_TEST(test_commit_is_counted);
    RUN_TEST(test_domains_are_independent);
    RUN_TEST(test_value_survives_power_cycle);
    RUN_TEST(test_reset_wipes_to_cold);
    return UNITY_END();
}
