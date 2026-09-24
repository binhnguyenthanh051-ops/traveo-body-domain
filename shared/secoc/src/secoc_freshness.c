/*
 * secoc_freshness.c — per-ID counter over a shared per-boot epoch, sender
 * generation + receiver accept rule + resync floor (ADR-0021 D3/D4/D5/D8).
 *
 * Host-testable: no vendor/RTOS/CAN/flash symbols; the only outward reach is the
 * injected secoc_freshness_store_if_t. MISRA C:2012 (target-buildable); guard
 * clauses deviate from 15.5 single-exit deliberately, matching the house style
 * in shared/crypto.
 *
 * @impl ADR-0021 D3 : freshness value, per-ID counter, commit-before-spend, rollover
 * @impl ADR-0021 D4 : receiver floor + per-ID high-water accept rule
 * @impl ADR-0021 D5 : receiver-reset resync (floor raise + sender floor adoption)
 * @impl ADR-0021 D8 : durable persistence via the freshness-store port
 */
#include "secoc_freshness.h"

/* Locate the slot for can_id among the registered ids. */
static bool find_slot(const uint16_t *ids, size_t n, uint16_t can_id, size_t *idx)
{
    for (size_t i = 0U; i < n; ++i)
    {
        if (ids[i] == can_id)
        {
            *idx = i;
            return true;
        }
    }
    return false;
}

static bool store_ok(const secoc_freshness_store_if_t *s)
{
    return (s != NULL) && (s->load != NULL) && (s->commit != NULL);
}

/* ===================================================================
 * Sender
 * =================================================================== */

/* @impl REQ-SECOC-006 : load(E) -> commit(E+1) durable -> spend E+1 */
bool secoc_tx_boot(secoc_tx_ctx_t *ctx, const secoc_freshness_store_if_t *store,
                   uint8_t domain, const uint16_t *ids, size_t id_count)
{
    if ((ctx == NULL) || !store_ok(store) || (ids == NULL))
    {
        return false;
    }
    if ((id_count == 0U) || (id_count > (size_t)SECOC_MAX_IDS))
    {
        return false;
    }

    uint16_t e = 0U;
    if (!store->load(domain, &e))
    {
        e = 0U;                         /* cold: never written */
    }
    uint16_t next_epoch = (uint16_t)(e + 1U);
    if (!store->commit(domain, next_epoch))
    {
        return false;                   /* store unusable => caller fails safe */
    }

    ctx->store = store;
    ctx->domain = domain;
    ctx->epoch = next_epoch;
    ctx->id_count = id_count;
    for (size_t i = 0U; i < id_count; ++i)
    {
        ctx->ids[i] = ids[i];
        ctx->counter[i] = 0U;
    }
    return true;
}

/* @impl REQ-SECOC-005 : per-ID counter
 * @impl REQ-SECOC-009 : rollover bumps the epoch (durable) and restarts counters */
bool secoc_tx_next(secoc_tx_ctx_t *ctx, uint16_t can_id,
                   uint16_t *epoch, uint16_t *counter)
{
    if ((ctx == NULL) || (epoch == NULL) || (counter == NULL))
    {
        return false;
    }
    size_t idx = 0U;
    if (!find_slot(ctx->ids, ctx->id_count, can_id, &idx))
    {
        return false;
    }

    if (ctx->counter[idx] == 0xFFFFU)
    {
        uint16_t bumped = (uint16_t)(ctx->epoch + 1U);
        if (!ctx->store->commit(ctx->domain, bumped))
        {
            return false;
        }
        ctx->epoch = bumped;
        for (size_t i = 0U; i < ctx->id_count; ++i)
        {
            ctx->counter[i] = 0U;
        }
    }

    uint16_t next = (uint16_t)(ctx->counter[idx] + 1U);
    ctx->counter[idx] = next;
    *epoch = ctx->epoch;
    *counter = next;
    return true;
}

/* @impl REQ-SECOC-008 : adopt an authenticated FRESHNESS_SYNC floor (forward only) */
bool secoc_tx_adopt_floor(secoc_tx_ctx_t *ctx, uint16_t requested_floor)
{
    if (ctx == NULL)
    {
        return false;
    }
    if (requested_floor <= ctx->epoch)
    {
        return false;                   /* never move backward */
    }
    if (!ctx->store->commit(ctx->domain, requested_floor))
    {
        return false;
    }
    ctx->epoch = requested_floor;
    for (size_t i = 0U; i < ctx->id_count; ++i)
    {
        ctx->counter[i] = 0U;           /* new epoch => counters restart */
    }
    return true;
}

/* ===================================================================
 * Receiver
 * =================================================================== */

/* @impl REQ-SECOC-008 : floor = persisted+1 (reads store; persistence is on accept) */
bool secoc_rx_boot(secoc_rx_ctx_t *ctx, const secoc_freshness_store_if_t *store,
                   uint8_t domain, const uint16_t *ids, size_t id_count)
{
    if ((ctx == NULL) || !store_ok(store) || (ids == NULL))
    {
        return false;
    }
    if ((id_count == 0U) || (id_count > (size_t)SECOC_MAX_IDS))
    {
        return false;
    }

    uint16_t h = 0U;
    if (!store->load(domain, &h))
    {
        h = 0U;                         /* cold */
    }

    ctx->store = store;
    ctx->domain = domain;
    ctx->floor = (uint16_t)(h + 1U);
    ctx->current = h;
    ctx->id_count = id_count;
    for (size_t i = 0U; i < id_count; ++i)
    {
        ctx->ids[i] = ids[i];
        ctx->high[i] = 0U;
    }
    return true;
}

/* @impl REQ-SECOC-007 : pure accept-rule check (no state mutation) */
secoc_fresh_verdict_t secoc_rx_check(const secoc_rx_ctx_t *ctx, uint16_t can_id,
                                     uint16_t epoch, uint16_t counter)
{
    size_t idx = 0U;
    if ((ctx == NULL) || !find_slot(ctx->ids, ctx->id_count, can_id, &idx))
    {
        return SECOC_FRESH_UNKNOWN;
    }
    if (epoch < ctx->floor)
    {
        return SECOC_FRESH_STALE;
    }
    if (epoch > ctx->current)
    {
        return SECOC_FRESH_OK;          /* newer epoch (adopted on accept) */
    }
    if ((epoch == ctx->current) && (counter > ctx->high[idx]))
    {
        return SECOC_FRESH_OK;
    }
    return SECOC_FRESH_STALE;
}

/* @impl REQ-SECOC-007 : commit accepted freshness; persist on epoch adoption */
void secoc_rx_accept(secoc_rx_ctx_t *ctx, uint16_t can_id,
                     uint16_t epoch, uint16_t counter)
{
    size_t idx = 0U;
    if ((ctx == NULL) || !find_slot(ctx->ids, ctx->id_count, can_id, &idx))
    {
        return;
    }

    if (epoch > ctx->current)
    {
        ctx->current = epoch;
        for (size_t i = 0U; i < ctx->id_count; ++i)
        {
            ctx->high[i] = 0U;          /* a new epoch starts every counter fresh */
        }
        (void)ctx->store->commit(ctx->domain, epoch);   /* durable: raises next-boot floor */
    }
    ctx->high[idx] = counter;
}
