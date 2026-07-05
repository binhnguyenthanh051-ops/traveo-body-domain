/*
 * sysmgr.c — resource & lifecycle manager (ADR-0015).
 *
 * Sequential bring-up: exactly one resource is "current" (the first
 * non-terminal entry in table order) at a time; the next entry is never
 * touched until the current one reaches a terminal state (READY, DEGRADED,
 * or FAILED after a SYSTEM_RESET escalation). Because deps must appear
 * earlier in the table (ADR-0015 D3), every dependency of the current
 * resource is already terminal by the time it is reached -- dependency
 * gating (D5) is just "every dep is READY", not a wait.
 */
#include "sysmgr.h"

#define SYSMGR_MAX_RESOURCES  16U

typedef struct {
    sysmgr_res_state_t state;
    uint8_t  attempts;
    bool     poll_started;   /* init() called for the current attempt; now waiting on poll_ready (or already resolved) */
    uint32_t started_ms;
} sysmgr_runtime_t;

static const sysmgr_resource_t *g_table;
static size_t g_count;
static sysmgr_reset_fn_t g_reset_fn;
static sysmgr_runtime_t g_rt[SYSMGR_MAX_RESOURCES];

void sysmgr_init(const sysmgr_resource_t *table, size_t count, sysmgr_reset_fn_t reset_fn)
{
    g_table = table;
    g_count = (count <= SYSMGR_MAX_RESOURCES) ? count : SYSMGR_MAX_RESOURCES;
    g_reset_fn = reset_fn;

    for (size_t i = 0U; i < g_count; ++i)
    {
        g_rt[i].state = SYSMGR_RES_UNINIT;
        g_rt[i].attempts = 0U;
        g_rt[i].poll_started = false;
        g_rt[i].started_ms = 0U;
    }
}

static size_t find_index(sysmgr_res_id_t id)
{
    for (size_t i = 0U; i < g_count; ++i)
    {
        if (g_table[i].id == id) { return i; }
    }
    return g_count;
}

static bool is_terminal(sysmgr_res_state_t s)
{
    return (s == SYSMGR_RES_READY) || (s == SYSMGR_RES_DEGRADED) || (s == SYSMGR_RES_FAILED);
}

static bool deps_satisfied(size_t idx)
{
    const sysmgr_resource_t *r = &g_table[idx];
    for (uint8_t d = 0U; d < r->dep_count; ++d)
    {
        size_t dep_idx = find_index(r->deps[d]);
        if ((dep_idx >= g_count) || (g_rt[dep_idx].state != SYSMGR_RES_READY))
        {
            return false;
        }
    }
    return true;
}

static bool any_dep_unready_terminal(size_t idx)
{
    const sysmgr_resource_t *r = &g_table[idx];
    for (uint8_t d = 0U; d < r->dep_count; ++d)
    {
        size_t dep_idx = find_index(r->deps[d]);
        if ((dep_idx < g_count) && is_terminal(g_rt[dep_idx].state) &&
            (g_rt[dep_idx].state != SYSMGR_RES_READY))
        {
            return true;
        }
    }
    return false;
}

static void handle_failure(size_t idx, uint32_t now_ms)
{
    const sysmgr_resource_t *r = &g_table[idx];
    uint32_t total_allowed = (uint32_t)r->max_retries + 1U;

    if (g_rt[idx].attempts < total_allowed)
    {
        g_rt[idx].state = SYSMGR_RES_INITIALIZING;
        g_rt[idx].poll_started = false;   /* due for another attempt next tick */
        return;
    }

    switch (r->on_exhausted)
    {
        case SYSMGR_FAIL_RETRY_INPLACE:
            g_rt[idx].attempts = 0U;      /* no terminal action -- keep retrying indefinitely */
            g_rt[idx].state = SYSMGR_RES_INITIALIZING;
            g_rt[idx].poll_started = false;
            break;
        case SYSMGR_FAIL_SYSTEM_RESET:
            if (g_reset_fn != NULL) { g_reset_fn(); }
            g_rt[idx].state = SYSMGR_RES_FAILED;
            break;
        case SYSMGR_FAIL_DEGRADE:
        default:
            g_rt[idx].state = SYSMGR_RES_DEGRADED;
            break;
    }
    (void)now_ms;
}

static void attempt_init(size_t idx, uint32_t now_ms)
{
    const sysmgr_resource_t *r = &g_table[idx];
    g_rt[idx].attempts++;
    g_rt[idx].started_ms = now_ms;

    int rc = r->init();
    if (rc == 0)
    {
        if (r->poll_ready == NULL)
        {
            g_rt[idx].state = SYSMGR_RES_READY;
        }
        else
        {
            g_rt[idx].state = SYSMGR_RES_INITIALIZING;
            g_rt[idx].poll_started = true;
        }
    }
    else
    {
        handle_failure(idx, now_ms);
    }
}

void sysmgr_tick(uint32_t now_ms)
{
    size_t idx = 0U;
    while ((idx < g_count) && is_terminal(g_rt[idx].state))
    {
        ++idx;
    }
    if (idx >= g_count) { return; }   /* settled */

    if (g_rt[idx].state == SYSMGR_RES_UNINIT)
    {
        if (!deps_satisfied(idx))
        {
            if (any_dep_unready_terminal(idx))
            {
                g_rt[idx].state = SYSMGR_RES_DEGRADED;   /* cascade, per D6/D7 */
            }
            return;
        }
        attempt_init(idx, now_ms);
        return;
    }

    /* SYSMGR_RES_INITIALIZING */
    const sysmgr_resource_t *r = &g_table[idx];
    if (!g_rt[idx].poll_started)
    {
        attempt_init(idx, now_ms);
        return;
    }

    if (r->poll_ready == NULL)
    {
        g_rt[idx].state = SYSMGR_RES_READY;   /* defensive; sync resources resolve inside attempt_init */
        return;
    }

    if (r->poll_ready() == 0)
    {
        g_rt[idx].state = SYSMGR_RES_READY;
        return;
    }

    if ((r->init_timeout_ms != 0U) && ((now_ms - g_rt[idx].started_ms) >= r->init_timeout_ms))
    {
        handle_failure(idx, now_ms);
    }
}

sysmgr_res_state_t sysmgr_res_state(sysmgr_res_id_t id)
{
    size_t idx = find_index(id);
    return (idx < g_count) ? g_rt[idx].state : SYSMGR_RES_UNINIT;
}

bool sysmgr_settled(void)
{
    for (size_t i = 0U; i < g_count; ++i)
    {
        if (!is_terminal(g_rt[i].state)) { return false; }
    }
    return true;
}
