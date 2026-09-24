/*
 * secoc_freshness_store_fake.c — volatile reference implementation of the SecOC
 * persistence port (ADR-0021 D8). See the header for the modelled contract.
 * Test harness — exempt from MISRA.
 */
#include "secoc_freshness_store_fake.h"

#define FAKE_MAX_DOMAINS   8U

static struct {
    bool     valid;      /* has this domain ever been committed? (else: cold ⇒ load false) */
    uint16_t epoch;
} g_slot[FAKE_MAX_DOMAINS];

static unsigned g_commit_count;

static bool fake_load(uint8_t domain, uint16_t *epoch)
{
    if ((domain >= FAKE_MAX_DOMAINS) || (epoch == NULL)) {
        return false;
    }
    if (!g_slot[domain].valid) {
        return false;                    /* cold: never written */
    }
    *epoch = g_slot[domain].epoch;
    return true;
}

static bool fake_commit(uint8_t domain, uint16_t epoch)
{
    if (domain >= FAKE_MAX_DOMAINS) {
        return false;
    }
    /* Durable-before-return: the write completes synchronously here, so by the
     * time this returns the value would survive a power_cycle. */
    g_slot[domain].valid = true;
    g_slot[domain].epoch = epoch;
    g_commit_count++;
    return true;
}

const secoc_freshness_store_if_t g_secoc_store_fake = {
    .load   = fake_load,
    .commit = fake_commit,
};

void secoc_store_fake_reset(void)
{
    for (unsigned i = 0U; i < FAKE_MAX_DOMAINS; ++i) {
        g_slot[i].valid = false;
        g_slot[i].epoch = 0U;
    }
    g_commit_count = 0U;
}

void secoc_store_fake_power_cycle(void)
{
    /* Stored values persist (that is the whole point); only the session's
     * commit tally resets, so a post-reboot test can count fresh commits. */
    g_commit_count = 0U;
}

unsigned secoc_store_fake_commit_count(void)
{
    return g_commit_count;
}
