/*
 * secoc_freshness_store_fake.h — host fake for the SecOC persistence port
 * (ADR-0021 D8). A volatile per-domain map that honours the contract the M6
 * eeprom_emu backing must also meet: commit is durable-before-return, load of a
 * never-written domain returns false (cold), domains are independent.
 *
 * `power_cycle` models a reboot: stored values SURVIVE (that is the point of
 * persistence), only the "session" ends. `reset` wipes storage back to cold.
 * `commit_count` lets a test assert the one-write-per-boot property (D3).
 * Test harness — exempt from MISRA.
 */
#ifndef SECOC_FRESHNESS_STORE_FAKE_H
#define SECOC_FRESHNESS_STORE_FAKE_H

#include "secoc_freshness.h"

extern const secoc_freshness_store_if_t g_secoc_store_fake;

void     secoc_store_fake_reset(void);         /* wipe all domains to cold, zero the counter */
void     secoc_store_fake_power_cycle(void);   /* keep stored values; only clears the counter */
unsigned secoc_store_fake_commit_count(void);  /* durable commits since last reset/power_cycle */

#endif /* SECOC_FRESHNESS_STORE_FAKE_H */
