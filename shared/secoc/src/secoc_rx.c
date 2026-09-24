/*
 * secoc_rx.c — the composed SecOC receive verdict (ADR-0023 D11). See header
 * for why this seam exists rather than emission at the primitives.
 *
 * One function per received frame owns the MAC verdict, the freshness verdict,
 * the accept, the per-reason drop counters AND the LOG_EVT_SECOC_* emission.
 * Counter and event are adjacent statements on a single path, which is what
 * makes REQ-LOG-009's "shall not diverge" structural instead of a promise.
 *
 * Host-testable: no vendor/RTOS/CAN symbols. tb_log.h is vendor-free (ADR-0001;
 * the third reach documented in secoc-architecture §7.1). MISRA C:2012 — the
 * guard clauses deviate from advisory 15.5 deliberately, matching secoc.c.
 *
 * @impl ADR-0023 D11 : one composed verdict seam; counters + events together
 * @impl REQ-LOG-009  : accept/reject/resync events on the verify path, with reason
 * @impl REQ-SECOC-001 : authenticate or drop before decode; count by reason
 * @impl REQ-SECOC-007 : MAC checked before freshness; a bad MAC never advances state
 * @impl REQ-SECOC-008 : authenticated floor adoption reports the completed resync
 * @impl ADR-0021 D9  : verify failure => no PDU emitted
 */
#include "secoc_rx.h"
#include "tb_log.h"
#include "log_events.h"

void secoc_rx_init(secoc_rx_t *rx, const secoc_crypto_t *cy, secoc_rx_ctx_t *fresh)
{
    if (rx == NULL)
    {
        return;
    }
    rx->cy = cy;
    rx->fresh = fresh;
    rx->n_accept = 0U;
    rx->n_drop_mac = 0U;
    rx->n_drop_fresh = 0U;
}

/* The two drop paths, factored so the counter and its event cannot be separated
 * by a later edit — that separation is exactly the failure REQ-LOG-009 forbids. */
static secoc_rx_verdict_t drop_mac(secoc_rx_t *rx, uint16_t can_id,
                                   secoc_verify_result_t reason)
{
    rx->n_drop_mac++;
    /* arg1 codes ARE secoc_verify_result_t (events.csv): 1 forged/corrupt,
     * 2 malformed length, 3 oracle error. The last one matters — a forgery test
     * that goes green because the M0+ is DEAD is a false pass, and "rejected"
     * alone cannot tell the two apart. */
    log_evt(LOG_EVT_SECOC_REJECT_MAC, (uint32_t)can_id, (uint16_t)reason);
    return SECOC_RX_DROP_MAC;
}

static secoc_rx_verdict_t drop_fresh(secoc_rx_t *rx, uint16_t can_id, uint16_t counter)
{
    rx->n_drop_fresh++;
    log_evt(LOG_EVT_SECOC_REJECT_FRESHNESS, (uint32_t)can_id, counter);
    return SECOC_RX_DROP_FRESHNESS;
}

secoc_rx_verdict_t secoc_rx_process(secoc_rx_t *rx, uint16_t can_id,
                                    const uint8_t *frame, size_t frame_len,
                                    uint8_t *pdu_out, size_t cap, size_t *pdu_len)
{
    if (rx == NULL)
    {
        return SECOC_RX_DROP_MAC;       /* nothing to count or report through */
    }
    if (pdu_len != NULL)
    {
        *pdu_len = 0U;                  /* no verdict yet => no PDU, from the first line */
    }
    if ((rx->cy == NULL) || (rx->fresh == NULL) || (pdu_out == NULL) || (pdu_len == NULL))
    {
        return drop_mac(rx, can_id, SECOC_MAC_ERROR);
    }

    uint16_t epoch = 0U;
    uint16_t counter = 0U;

    /* MAC FIRST. A frame that fails here must never reach the freshness state,
     * or an attacker advances the high-water with garbage (REQ-SECOC-007). */
    secoc_verify_result_t vr = secoc_verify(rx->cy, can_id, frame, frame_len,
                                            &epoch, &counter, pdu_out, cap, pdu_len);
    if (vr != SECOC_OK)
    {
        *pdu_len = 0U;                  /* drop-before-decode, restated (ADR-0021 D9) */
        return drop_mac(rx, can_id, vr);
    }

    secoc_fresh_verdict_t fv = secoc_rx_check(rx->fresh, can_id, epoch, counter);
    if (fv != SECOC_FRESH_OK)
    {
        /* STALE (replay) and UNKNOWN (an ID this receiver does not protect) share
         * one counter and one event: both are "authentic bytes we will not act
         * on", which is the granularity REQ-SECOC-001 counts at. The received
         * counter in arg1 is what makes a replay readable on the host. */
        *pdu_len = 0U;
        return drop_fresh(rx, can_id, counter);
    }

    /* Emit BEFORE committing the accept. The event timestamps the VERDICT, not
     * the persistence: secoc_rx_accept() durably commits on an epoch change, and
     * once eeprom_emu backs the store (M6) that is a flash write. Logging after
     * it would smear the accept's ts_ms by however long the write took, on the
     * one path M5 measures. */
    rx->n_accept++;
    log_evt(LOG_EVT_SECOC_ACCEPT, (uint32_t)can_id, counter);

    secoc_rx_accept(rx->fresh, can_id, epoch, counter);
    return SECOC_RX_ACCEPT;
}

bool secoc_rx_sync_adopt(secoc_tx_ctx_t *tx, uint16_t can_id, uint16_t requested_floor)
{
    if (tx == NULL)
    {
        return false;
    }
    bool moved = secoc_tx_adopt_floor(tx, requested_floor);
    if (moved)
    {
        /* Only on a real move. A duplicate sync, or one carrying a floor already
         * covered, is not a resync COMPLETING — reporting it would let the BVT
         * assert a recovery that never happened. */
        log_evt(LOG_EVT_SECOC_RESYNC, (uint32_t)can_id, tx->epoch);
    }
    return moved;
}
