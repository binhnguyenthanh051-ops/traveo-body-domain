/*
 * log_port_fake.c -- scripted host implementation of log_port.h (ADR-0023 D8).
 *
 * The reference behaviour of the port contract: this file IS the definition of
 * what a target port must do, in a form tests can drive. Test harness -- MISRA
 * does not apply (docs/coding-standard.md).
 */
#include "log_port_fake.h"
#include "log_port.h"

#include <string.h>

#define SINK_CAP    4096U

static uint8_t           s_ring[FAKE_RING_CAP];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;

static uint8_t  s_sink[SINK_CAP];
static size_t   s_sink_len;
static uint8_t  s_panic_sink[SINK_CAP];
static size_t   s_panic_len;

static uint32_t s_now_ms;
static uint32_t s_now_step;
static size_t   s_tx_accept;
static bool     s_sink_ready;
static bool     s_sink_idle;

static int32_t  s_lock_depth;
static uint32_t s_lock_max_depth;
static uint32_t s_lock_enter;
static uint32_t s_barriers;

/* ---- test hooks --------------------------------------------------- */

void log_fake_reset(void)
{
    memset(s_ring, 0, sizeof s_ring);
    s_head = 0U;
    s_tail = 0U;
    memset(s_sink, 0, sizeof s_sink);
    memset(s_panic_sink, 0, sizeof s_panic_sink);
    s_sink_len       = 0U;
    s_panic_len      = 0U;
    s_now_ms         = 0U;
    s_now_step       = 0U;
    s_tx_accept      = SIZE_MAX;
    s_sink_ready     = true;
    s_sink_idle      = true;
    s_lock_depth     = 0;
    s_lock_max_depth = 0U;
    s_lock_enter     = 0U;
    s_barriers       = 0U;
}

void log_fake_reset_sink_only(void)
{
    memset(s_sink, 0, sizeof s_sink);
    memset(s_panic_sink, 0, sizeof s_panic_sink);
    s_sink_len  = 0U;
    s_panic_len = 0U;
}

void log_fake_set_now(uint32_t ms)          { s_now_ms = ms; }
void log_fake_set_now_step(uint32_t step)   { s_now_step = step; }
void log_fake_set_tx_accept(size_t n)       { s_tx_accept = n; }
void log_fake_set_sink_ready(bool ready)    { s_sink_ready = ready; }
void log_fake_set_sink_idle(bool idle)      { s_sink_idle = idle; }

const uint8_t *log_fake_sink(void)          { return s_sink; }
size_t         log_fake_sink_len(void)      { return s_sink_len; }
const uint8_t *log_fake_panic_sink(void)    { return s_panic_sink; }
size_t         log_fake_panic_sink_len(void){ return s_panic_len; }

bool     log_fake_lock_balanced(void)   { return s_lock_depth == 0; }
uint32_t log_fake_lock_max_depth(void)  { return s_lock_max_depth; }
uint32_t log_fake_lock_enter_count(void){ return s_lock_enter; }
uint32_t log_fake_barrier_count(void)   { return s_barriers; }

size_t log_fake_ring_used(void)
{
    return (size_t)((s_head - s_tail) & (FAKE_RING_CAP - 1U));
}

/* ---- the port contract -------------------------------------------- */

uint8_t *log_port_ring(void)      { return s_ring; }
size_t   log_port_ring_cap(void)  { return FAKE_RING_CAP; }

volatile uint32_t *log_port_head(void) { return &s_head; }
volatile uint32_t *log_port_tail(void) { return &s_tail; }

void log_port_publish_barrier(void) { s_barriers++; }

uint32_t log_port_lock(void)
{
    s_lock_depth++;
    s_lock_enter++;
    if ((uint32_t)s_lock_depth > s_lock_max_depth) {
        s_lock_max_depth = (uint32_t)s_lock_depth;
    }
    return 0U;   /* previous mask state; irrelevant on host */
}

void log_port_unlock(uint32_t state)
{
    (void)state;
    s_lock_depth--;
}

uint32_t log_port_now_ms(void)
{
    const uint32_t now = s_now_ms;
    s_now_ms += s_now_step;
    return now;
}

size_t log_port_tx(const uint8_t *data, size_t len)
{
    size_t n;

    if (!s_sink_ready) {
        return 0U;
    }
    n = (len < s_tx_accept) ? len : s_tx_accept;
    if ((s_sink_len + n) > SINK_CAP) {
        n = SINK_CAP - s_sink_len;
    }
    memcpy(&s_sink[s_sink_len], data, n);
    s_sink_len += n;
    return n;
}

void log_port_tx_blocking(const uint8_t *data, size_t len)
{
    size_t n = len;

    if ((s_panic_len + n) > SINK_CAP) {
        n = SINK_CAP - s_panic_len;
    }
    memcpy(&s_panic_sink[s_panic_len], data, n);
    s_panic_len += n;
}

bool log_port_sink_ready(void) { return s_sink_ready; }
bool log_port_sink_idle(void)  { return s_sink_idle; }
