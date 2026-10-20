/*
 * wflash_fake.h — host fake of TRAVEO T2G work flash for the freshness store
 * (ADR-0024). Models what the store's power-loss argument depends on:
 *
 *   - word states: ERASED, PROGRAMMED, TORN (32-bit program unit, TRM §9)
 *   - no reprogramming a word without erasing its sector (counted as a violation)
 *   - blank_check: 1 only if every word in range is ERASED
 *   - read_checked (the DMA read): fails on an ERASED word (reading erased
 *     work flash raises an ECC error, TRM §9.3.1) and on a TORN word in
 *     TORN_ECC mode; in TORN_GARBAGE mode a torn word reads as wrong bits
 *   - read (a CPU read) is never expected: every call is counted
 *   - power loss: after N erase/program steps the next one is interrupted
 *     (its sector or word becomes TORN) and everything after fails until
 *     power_cycle()
 *
 * Test harness — exempt from MISRA.
 */
#ifndef WFLASH_FAKE_H
#define WFLASH_FAKE_H

#include "hal.h"

#define WF_BASE         0x14018000U     /* Node B small-sector region (cyt4bf8cds.h) */
#define WF_SECTOR_SIZE  128U
#define WF_SECTORS      4U

typedef enum {
    WF_TORN_ECC     = 0,    /* a torn word fails the checked read (uncorrectable ECC) */
    WF_TORN_GARBAGE = 1     /* a torn word reads back as wrong bits, no ECC error */
} wf_torn_mode_t;

extern const hal_flash_if_t g_wflash_fake;

void     wflash_fake_reset(void);                  /* all erased, powered, no limits */
void     wflash_fake_set_torn_mode(wf_torn_mode_t mode);
void     wflash_fake_fail_after(int steps);        /* steps complete, the next is interrupted */
void     wflash_fake_power_cycle(void);            /* power back, no limit; contents persist */
void     wflash_fake_decay_word(uint32_t addr);    /* a programmed word goes TORN (retention) */
unsigned wflash_fake_steps(void);                  /* erase/program steps attempted */
unsigned wflash_fake_erase_count(uint32_t sector_addr);
unsigned wflash_fake_cpu_reads(void);              /* must stay 0 */
unsigned wflash_fake_reprogram_violations(void);   /* must stay 0 */

#endif /* WFLASH_FAKE_H */
