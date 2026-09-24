/*
 * secoc_rx.h — the composed SecOC receive verdict: MAC + freshness + accept,
 * with the per-reason drop counters and the LOG_EVT_SECOC_* events emitted
 * from the SAME place.
 *
 * Decisions: ADR-0023 D11 (this seam exists and why), ADR-0021 D1/D4/D5/D9.
 * Requirements: REQ-SECOC-001, REQ-SECOC-007, REQ-LOG-009.
 *
 * -------------------------------------------------------------------------
 * Why a seam rather than emission at the primitives
 *
 * No function below this one knows a verdict. secoc_verify() knows the MAC
 * failed but cannot know a frame is ACCEPTED — the freshness gate may still
 * reject it. secoc_rx_check() is deliberately PURE, so an emission inside it
 * would be a hidden side effect (and would fire in test_secoc_freshness, where
 * no ring exists). secoc_rx_accept() knows a frame passed, not why the others
 * did not.
 *
 * REQ-LOG-009 requires that these events and the REQ-SECOC-001 per-reason drop
 * counters "shall not diverge". The only way to make that structural rather
 * than a review promise is for one function to own both — so the counter
 * increment and the log_evt() are adjacent statements, on the one path, with
 * nothing between them to skip.
 *
 * -------------------------------------------------------------------------
 * Layering (ADR-0001)
 *
 * Still host-testable, still vendor-free. This file adds a THIRD outward reach
 * to shared/secoc beyond the MAC oracle and the store port: tb_log.h. That
 * header has no vendor/RTOS dependency and is itself host-tested, so the rule
 * holds — but secoc-architecture §7.1 said "exactly two", so ADR-0023 D11
 * amends it deliberately rather than letting it rot. The log PORT is not
 * reachable from here: binding a sink is the image's job, not SecOC's.
 *
 * What stays in the app: body_decode()/unpack_* and the decode drop counter.
 * Those name message IDs, which is app composition — and per REQ-LOG-009 the
 * events must fire on the verify path, i.e. BEFORE decode runs at all.
 */
#ifndef SECOC_RX_H
#define SECOC_RX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "secoc.h"             /* secoc_crypto_t, secoc_verify_result_t */
#include "secoc_freshness.h"   /* secoc_rx_ctx_t, secoc_tx_ctx_t */

/* -------------------------------------------------------------------
 * Verdict — the three outcomes a receiver can have for one secured frame.
 *
 * Coarser than the underlying result enums ON PURPOSE: this is the granularity
 * REQ-SECOC-001 counts at (bad-MAC vs stale-freshness) and the granularity the
 * BVT asserts at. The finer sub-reason survives in the event's arg1, where it
 * costs nothing, rather than in a wider enum every call site must switch on.
 * ----------------------------------------------------------------- */
typedef enum {
    SECOC_RX_ACCEPT         = 0,   /* MAC valid AND fresh; pdu_out/pdu_len valid */
    SECOC_RX_DROP_MAC       = 1,   /* bad MAC, malformed length, or oracle failure */
    SECOC_RX_DROP_FRESHNESS = 2    /* replay/stale epoch, or an unregistered CAN ID */
} secoc_rx_verdict_t;

/* -------------------------------------------------------------------
 * Receiver instance: the crypto binding, the freshness state, and the counters
 * that REQ-SECOC-001 requires — all reachable from a debugger, all incremented
 * in the same statement as the matching event.
 *
 * Counters are plain uint32_t, not volatile: they are written and read from the
 * task that owns this context. A node exposing them to a debugger or telemetry
 * publishes its own volatile mirrors (see each node's secoc_app.c).
 * ----------------------------------------------------------------- */
typedef struct {
    const secoc_crypto_t *cy;      /* MAC oracle + key_id (ADR-0021 D7) */
    secoc_rx_ctx_t       *fresh;   /* freshness receiver context (ADR-0021 D4) */
    uint32_t n_accept;             /* frames released to decode */
    uint32_t n_drop_mac;           /* REQ-SECOC-001: bad-MAC / malformed / unknown-key */
    uint32_t n_drop_fresh;         /* REQ-SECOC-001: stale freshness (replay) */
} secoc_rx_t;

/* Bind a receiver and zero its counters. `cy` and `fresh` must outlive `rx`
 * (both are statics in practice). A NULL argument leaves the instance in a
 * state where every frame is dropped as SECOC_RX_DROP_MAC — fail-safe, and
 * loud, because each drop still emits its event. */
void secoc_rx_init(secoc_rx_t *rx, const secoc_crypto_t *cy, secoc_rx_ctx_t *fresh);

/* -------------------------------------------------------------------
 * Process ONE received secured frame (REQ-SECOC-001, 007; REQ-LOG-009).
 *
 * MAC first, then freshness — a bad MAC must never advance freshness state, or
 * an attacker moves the high-water with garbage (REQ-SECOC-007). On ACCEPT the
 * stripped authentic PDU is copied to pdu_out and *pdu_len set; on either drop
 * NOTHING is written to pdu_out and *pdu_len is set to 0, so a caller that
 * ignores the verdict still cannot actuate on unauthenticated bytes
 * (drop-before-decode, ADR-0021 D9).
 *
 * Emits EXACTLY ONE event per call — accept, reject-MAC, or reject-freshness —
 * and increments exactly one counter, adjacent to it. This is a per-frame hot
 * path: log_evt() is bounded and non-blocking (ADR-0023 D7), and nothing else
 * belongs here.
 *
 * Calls the MAC oracle, so on target this runs from task context, not an ISR.
 * ----------------------------------------------------------------- */
secoc_rx_verdict_t secoc_rx_process(secoc_rx_t *rx, uint16_t can_id,
                                    const uint8_t *frame, size_t frame_len,
                                    uint8_t *pdu_out, size_t cap, size_t *pdu_len);

/* -------------------------------------------------------------------
 * The RX action for an authenticated FRESHNESS_SYNC (ADR-0021 D5 / REQ-SECOC-008).
 *
 * Lives here, with the other verdict emissions, because LOG_EVT_SECOC_RESYNC is
 * a SecOC verdict like the rest — and because it is the one event that does NOT
 * fire on the actuator. B only SENDS the sync; the resync COMPLETES when the
 * gateway raises its sender epoch, so this runs on Node A.
 *
 * Adopts epoch = max(current, requested_floor) into `tx` and emits RESYNC
 * (arg0 = the sync's CAN ID, arg1 = the new epoch) IFF the epoch actually
 * moved. A duplicate or already-covered floor is not a resync completing, and
 * counting it as one would let the BVT assert a recovery that never happened.
 *
 * Call it only on a frame that has already passed secoc_rx_process() — an
 * unauthenticated floor bump is exactly the attack D5 is built to refuse.
 * Returns true iff the epoch moved (i.e. iff an event was emitted).
 * ----------------------------------------------------------------- */
bool secoc_rx_sync_adopt(secoc_tx_ctx_t *tx, uint16_t can_id, uint16_t requested_floor);

#endif /* SECOC_RX_H */
