/*
 * secoc_store.h — persistent SecOC freshness store: a two-slot record in work
 * flash (ADR-0024), behind the ADR-0021 D8 port (secoc_freshness_store_if_t).
 *
 * One record per node carries both persisted domains (the node's own sender
 * epoch and the floor for its peer). A save erases the OLDER slot, programs the
 * body, then the commit mark, reads it back, and only then returns: a power loss
 * at any step leaves the previous record as the newest valid one (D2). A mount
 * blank-checks each slot's mark, then reads the whole record through the HAL's
 * fault-safe read (DMA on target), so no state of work flash -- erased, torn or
 * decayed -- can fault the CPU (D3).
 *
 * Host-testable (ADR-0001): flash only through hal_flash_if_t, which must
 * provide erase_sector, write, blank_check and read_checked. The CPU read
 * (`read`) is never used.
 *
 * Single instance per image (the D8 port has no context argument). The target
 * port serializes commit() across tasks (ADR-0024 D5); this module does not lock.
 */
#ifndef SECOC_STORE_H
#define SECOC_STORE_H

#include <stdint.h>
#include "hal.h"
#include "secoc_freshness.h"

/* Domains in one record (ADR-0024 D1): 0 = own sender epoch, 1 = peer floor. */
#define SECOC_STORE_DOMAINS     2U
/* Bytes per record: seq, epochs, written bitmap, CRC-32, commit mark. */
#define SECOC_STORE_RECORD_LEN  20U
/* Last word of a record, programmed last (D2). */
#define SECOC_STORE_COMMIT_MARK 0xA5C33C5AU

typedef struct {
    const hal_flash_if_t *flash;
    uint32_t slot_addr[2];      /* two distinct, sector-aligned work-flash sectors */
} secoc_store_cfg_t;

typedef enum {
    SECOC_STORE_OK      = 0,    /* a valid record was found */
    SECOC_STORE_FACTORY = 1,    /* both slots blank: every domain loads cold */
    SECOC_STORE_BROKEN  = 2     /* no valid record and not blank, or a bad config */
} secoc_store_status_t;

/* Scan both slots and load the newest valid record into the RAM mirror (D3).
 * Call once at boot, before secoc_tx_boot/secoc_rx_boot.
 *
 * BROKEN is fail-closed: load() and commit() then return false. The caller MUST
 * NOT start SecOC on BROKEN. load() alone can't say "broken" (false means cold
 * in the D8 port), and a receiver booted cold would accept every old epoch. */
secoc_store_status_t secoc_store_mount(const secoc_store_cfg_t *cfg);

/* The D8 port over the mounted store. */
extern const secoc_freshness_store_if_t g_secoc_store;

#endif /* SECOC_STORE_H */
