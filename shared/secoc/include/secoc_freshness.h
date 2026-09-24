/*
 * secoc_freshness.h — SecOC freshness: per-ID counter over a shared per-boot
 * epoch, sender generation + receiver accept rule, and the persistence port.
 *
 * Decisions: ADR-0021 D3 (freshness value + persist order + rollover), D4
 * (receiver accept rule), D5 (resync floor), D8 (store port). Requirements:
 * REQ-SECOC-005..009.
 *
 * Host-testable (ADR-0001): no vendor headers, no FreeRTOS, no CAN/flash symbols.
 * The only outward reach is the injected secoc_freshness_store_if_t port — the
 * eeprom_emu backing (M6) hides behind it; host tests bind a fake.
 */
#ifndef SECOC_FRESHNESS_H
#define SECOC_FRESHNESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Max protected CAN IDs a sender or receiver context tracks (config, ADR-0004
 * style). Bump per node; a body network needs a handful. */
#ifndef SECOC_MAX_IDS
#define SECOC_MAX_IDS   8U
#endif

/* -------------------------------------------------------------------
 * Persistence port (ADR-0021 D8 / REQ-SECOC-006).
 *
 * `domain` selects the persisted slot: a node's own sender epoch, or a
 * receiver's accepted floor per remote sender. `commit` MUST be durable before
 * it returns — the whole anti-reuse argument (commit E+1 before spending E)
 * rests on it. `load` returns false when the slot has never been written (cold
 * boot) so the caller can seed from 0.
 * ----------------------------------------------------------------- */
typedef struct {
    bool (*load)(uint8_t domain, uint16_t *epoch);
    bool (*commit)(uint8_t domain, uint16_t epoch);   /* durable-before-return */
} secoc_freshness_store_if_t;

/* ===================================================================
 * Sender side (ADR-0021 D3)
 * =================================================================== */
typedef struct {
    const secoc_freshness_store_if_t *store;
    uint8_t  domain;                    /* persisted slot for this node's sender epoch */
    uint16_t epoch;                     /* epoch currently spent on outgoing frames */
    uint16_t ids[SECOC_MAX_IDS];        /* registered protected CAN IDs */
    uint16_t counter[SECOC_MAX_IDS];    /* per-ID counter, RAM only (D3) */
    size_t   id_count;
} secoc_tx_ctx_t;

/* Boot a sender (REQ-SECOC-006): load(E) — cold ⇒ E=0 — then commit(E+1)
 * DURABLY, then adopt E+1 for use. Registers the protected `ids`. Returns false
 * if the store is unusable (commit failed) ⇒ the caller must fail-safe and send
 * nothing authenticated. */
bool secoc_tx_boot(secoc_tx_ctx_t *ctx, const secoc_freshness_store_if_t *store,
                   uint8_t domain, const uint16_t *ids, size_t id_count);

/* Next freshness for `can_id` (REQ-SECOC-005): returns the (epoch,counter) to
 * put on the wire, then advances the per-ID counter. On counter rollover past
 * 0xFFFF it bumps the epoch instead (commit(E+1) durable, reset all counters —
 * REQ-SECOC-009). Returns false if `can_id` is not registered or a required
 * commit failed. */
bool secoc_tx_next(secoc_tx_ctx_t *ctx, uint16_t can_id,
                   uint16_t *epoch, uint16_t *counter);

/* Adopt a floor requested by an authenticated FRESHNESS_SYNC (ADR-0021 D5 /
 * REQ-SECOC-008): epoch = max(epoch, requested_floor). A bump is a real epoch
 * advance, so it is persisted durably (commit) and resets all per-ID counters,
 * exactly like a rollover bump. Never moves backward. Returns true iff the
 * epoch moved. */
bool secoc_tx_adopt_floor(secoc_tx_ctx_t *ctx, uint16_t requested_floor);

/* ===================================================================
 * Receiver side (ADR-0021 D4)
 * =================================================================== */
typedef enum {
    SECOC_FRESH_OK      = 0,   /* accept (caller then calls secoc_rx_accept) */
    SECOC_FRESH_STALE   = 1,   /* epoch < floor, or counter ≤ high-water: reject */
    SECOC_FRESH_UNKNOWN = 2    /* can_id not registered on this receiver */
} secoc_fresh_verdict_t;

typedef struct {
    const secoc_freshness_store_if_t *store;
    uint8_t  domain;                    /* persisted slot: highest epoch this rx has accepted */
    uint16_t floor;                     /* fixed at boot = persisted+1; rejects the prior epoch */
    uint16_t current;                   /* highest epoch accepted so far (== persisted value) */
    uint16_t ids[SECOC_MAX_IDS];
    uint16_t high[SECOC_MAX_IDS];       /* per-ID high-water, RAM only (lost on reboot ⇒ D5) */
    size_t   id_count;
} secoc_rx_ctx_t;

/* Boot a receiver (ADR-0021 D5 / REQ-SECOC-008): load H = highest previously
 * accepted epoch (cold ⇒ H = 0); floor = H + 1; current = H. `floor` is fixed
 * for the session and rejects the entire prior epoch — that is what closes the
 * receiver-reset replay window (the RAM high-water is gone, so an in-epoch
 * replay must be barred by epoch). The caller then emits FRESHNESS_SYNC. This
 * only READS the store (persistence happens on accept). Returns false on store
 * error. */
bool secoc_rx_boot(secoc_rx_ctx_t *ctx, const secoc_freshness_store_if_t *store,
                   uint8_t domain, const uint16_t *ids, size_t id_count);

/* Check a frame's freshness (REQ-SECOC-007). PURE — does not mutate state, so
 * it can never advance high-water; call it only AFTER the MAC has verified, and
 * apply the result with secoc_rx_accept. Rules (per D4):
 *   epoch < floor                        -> STALE
 *   epoch > current                      -> OK   (a newer epoch; adopted on accept)
 *   epoch == current, counter > high[id] -> OK
 *   otherwise                            -> STALE
 *   can_id not registered                -> UNKNOWN */
secoc_fresh_verdict_t secoc_rx_check(const secoc_rx_ctx_t *ctx, uint16_t can_id,
                                     uint16_t epoch, uint16_t counter);

/* Commit an accepted (epoch,counter) into receiver state (REQ-SECOC-007/008).
 * If epoch > current, adopt it: current = epoch, reset ALL per-ID high-water
 * (a new epoch starts every counter fresh), and PERSIST the new epoch durably
 * (commit) so a later reboot raises floor above it — this is the durable part
 * of the receiver-reset guard, and it fires once per new epoch (rare), not per
 * frame. Then high[id] = counter. Call ONLY after MAC + _check both passed.
 * No-op for an unknown can_id. */
void secoc_rx_accept(secoc_rx_ctx_t *ctx, uint16_t can_id,
                     uint16_t epoch, uint16_t counter);

#endif /* SECOC_FRESHNESS_H */
