/*
 * log.c -- target logging channel, host-testable core (ADR-0023, REQ-LOG-001..014).
 *
 * STUB: signatures + contracts only, bodies to be filled (docs/workflow.md --
 * Claude Code owns the contract, Copilot fills the body, Claude Code reviews).
 * test_log is therefore RED until it is implemented. That is intentional: the
 * suite pins the contract before the code exists.
 *
 * MISRA C:2012 applies here (ADR-0003) -- fixed-width types, no <stdio.h>, no
 * varargs, explicit braces, no implicit conversions.
 *
 * @impl ADR-0023      : structured-event logging channel
 * @impl REQ-LOG-001   : no formatting on target -- fixed-size binary records
 * @impl REQ-LOG-011   : pure logic; all hardware behind log_port_*
 */
#include "log.h"
#include "log_port.h"

#include <string.h>

/* Per-IMAGE state. shared/log is compiled separately into the app-core image
 * and the CM0+ image, so each core gets its own copy of these statics -- the
 * file-static core id is correct, not a shortcut. */
static log_core_t s_core = LOG_CORE_APP;
static uint8_t    s_seq[LOG_CORE_COUNT];
static uint32_t   s_dropped[LOG_CORE_COUNT];
static uint32_t   s_pending_overflow[LOG_CORE_COUNT];

enum {
    LOG_EVT_OVERFLOW = 0x0004U
};

/* ------------------------------------------------------------------ */

static void log_wr16(uint8_t *out, size_t off, uint16_t v)
{
    out[off]     = (uint8_t)(v & 0x00FFU);
    out[off + 1U] = (uint8_t)((v >> 8U) & 0x00FFU);
}

static void log_wr32(uint8_t *out, size_t off, uint32_t v)
{
    out[off]      = (uint8_t)(v & 0x000000FFUL);
    out[off + 1U] = (uint8_t)((v >> 8U) & 0x000000FFUL);
    out[off + 2U] = (uint8_t)((v >> 16U) & 0x000000FFUL);
    out[off + 3U] = (uint8_t)((v >> 24U) & 0x000000FFUL);
}

static size_t log_min_size(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

static void log_ring_copy_in(log_core_t core, uint32_t start, const uint8_t *src, size_t len)
{
    uint8_t *ring = log_port_ring(core);
    size_t   cap  = log_port_ring_cap(core);
    size_t   mask = cap - 1U;
    size_t   idx  = (size_t)start & mask;
    size_t   first;

    if (len == 0U) {
        return;
    }

    first = log_min_size(len, cap - idx);
    (void)memcpy(&ring[idx], src, first);

    if (len > first) {
        (void)memcpy(&ring[0], &src[first], len - first);
    }
}

/* Reserve, copy and publish -- ALL inside the masked region (REQ-LOG-005).
 *
 * The copy cannot be moved outside the lock, even though that looks cheaper.
 * With two producers on one core (a task and an ISR both calling log_evt),
 * releasing the lock after reserving lets this interleave:
 *
 *   task reserves [0,16)                      -- preempted before copying
 *   ISR  reserves [16,32), copies, head = 32  -- publishes bytes 0..15 that
 *                                                have not been written yet
 *   task copies [0,16), head = 16             -- head moves BACKWARDS
 *
 * The first fault ships a garbage record; the second is worse -- `head - tail`
 * underflows and the drain treats the entire ring as pending, flooding the
 * sink. Publishing under the lock makes reserve+copy+publish one indivisible
 * step, so head only ever advances and never names unwritten bytes.
 *
 * Bounded by construction: LOG_REC_SIZE (16) per record, LOG_TEXT_MAX (64) for
 * text -- so the masked region stays short even though it now spans the copy.
 */
static bool log_try_enqueue(log_core_t core,
                            const uint8_t *first, size_t first_len,
                            const uint8_t *second, size_t second_len)
{
    volatile uint32_t * const       head_cell = log_port_head(core);
    volatile uint32_t const * const tail_cell = log_port_tail(core);
    const size_t                    cap       = log_port_ring_cap(core);
    const size_t                    need      = first_len + second_len;
    uint32_t                        start;
    uint32_t                        state;
    size_t                          used;

    if (need == 0U) {
        return true;
    }

    state = log_port_lock();

    start = *head_cell;
    used  = (size_t)(start - *tail_cell);
    if ((need > cap) || ((cap - used) < need)) {
        log_port_unlock(state);
        return false;
    }

    log_ring_copy_in(core, start, first, first_len);
    log_ring_copy_in(core, start + (uint32_t)first_len, second, second_len);

    log_port_publish_barrier();
    *head_cell = start + (uint32_t)need;

    log_port_unlock(state);
    return true;
}

static void log_note_drop(log_core_t core)
{
    s_dropped[core]++;
    s_pending_overflow[core]++;
}

/* @impl REQ-LOG-002 : CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF, no
 *                     reflection, no final XOR. Check value for "123456789"
 *                     is 0x29B1 (pinned by test_log). */
uint16_t log_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;
    size_t   i;
    uint8_t  bit;

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

/* @impl REQ-LOG-002 : explicit byte-offset encoding, little-endian, never a
 *                     struct overlay. Layout is in log_types.h. */
void log_rec_encode(uint8_t *out, log_core_t core, uint8_t seq,
                    uint16_t evt, uint32_t ts_ms, uint32_t arg0, uint16_t arg1)
{
    uint16_t crc16;

    out[LOG_OFF_SYNC]     = LOG_SYNC;
    out[LOG_OFF_CORE_SEQ] = (uint8_t)(((uint8_t)core << LOG_CORE_SHIFT) | (seq & LOG_SEQ_MASK));
    log_wr16(out, LOG_OFF_EVT, evt);
    log_wr32(out, LOG_OFF_TS_MS, ts_ms);
    log_wr32(out, LOG_OFF_ARG0, arg0);
    log_wr16(out, LOG_OFF_ARG1, arg1);
    crc16 = log_crc16(out, LOG_OFF_CRC);
    log_wr16(out, LOG_OFF_CRC, crc16);
}

/* ------------------------------------------------------------------ */

void log_init(log_core_t core)
{
    s_core                     = core;
    s_seq[core]                = 0U;
    s_dropped[core]            = 0U;
    s_pending_overflow[core]   = 0U;
    *log_port_head(core)       = 0U;
    *log_port_tail(core)       = 0U;

    /* Note: resets THIS core's ring only -- never the other core's (D4). */

    /* Banner on the UART-owning core only. Delegates to log_banner() rather
     * than carrying a second copy of the string -- two literals drift. */
    if (core == LOG_CORE_APP) {
        log_banner();
    }
}

/* @impl REQ-LOG-005 : bounded, non-blocking, ISR-safe; the masked region spans
 *                     reserve+copy+publish as one indivisible step, bounded by
 *                     LOG_REC_SIZE (see log_try_enqueue for why).
 * @impl REQ-LOG-006 : no room => drop the NEWEST, count it, arm an overflow
 *                     report for when space frees.
 * @impl ADR-0023 D10 : ts_ms is sampled HERE, at production time -- never at
 *                     drain time. That is what keeps the recorded timeline
 *                     truthful despite cyclic output. */
void log_evt(uint16_t evt, uint32_t arg0, uint16_t arg1)
{
    uint8_t  overflow_rec[LOG_REC_SIZE] = { 0U };
    uint8_t  event_rec[LOG_REC_SIZE];
    uint32_t now_ms;
    uint8_t  seq;
    size_t   overflow_len = 0U;

    now_ms = log_port_now_ms();
    seq    = s_seq[s_core];

    if (s_pending_overflow[s_core] != 0U) {
        log_rec_encode(overflow_rec, s_core, seq, LOG_EVT_OVERFLOW, now_ms, s_pending_overflow[s_core], 0U);
        overflow_len = LOG_REC_SIZE;
        seq = (uint8_t)((seq + 1U) & LOG_SEQ_MASK);
    }

    log_rec_encode(event_rec, s_core, seq, evt, now_ms, arg0, arg1);

    if (!log_try_enqueue(s_core,
                         (overflow_len != 0U) ? overflow_rec : NULL,
                         overflow_len,
                         event_rec,
                         LOG_REC_SIZE)) {
        log_note_drop(s_core);
        return;
    }

    if (overflow_len != 0U) {
        s_seq[s_core] = (uint8_t)((s_seq[s_core] + 2U) & LOG_SEQ_MASK);
        s_pending_overflow[s_core] = 0U;
    } else {
        s_seq[s_core] = (uint8_t)((s_seq[s_core] + 1U) & LOG_SEQ_MASK);
    }
}

/* @impl REQ-LOG-003 : ASCII only -- a high-bit byte would forge LOG_SYNC.
 * @impl REQ-LOG-005 : truncated at LOG_TEXT_MAX so the masked region in
 *                     log_try_enqueue stays bounded. */
void log_text(const char *s)
{
    size_t len;

    if (s == NULL) {
        return;
    }

    /* Bound checked before every dereference, and exactly one dereference per
     * iteration -- keeps the loop obviously in-range for a reader and for
     * static analysis. */
    len = 0U;
    while (len < LOG_TEXT_MAX) {
        /* `s` is a NUL-terminated C string by contract; the loop stops at the
         * NUL and LOG_TEXT_MAX only caps how far it will look. cppcheck cannot
         * see callers from this translation unit, so it assumes an arbitrary
         * shorter array and flags the bound as possibly redundant. */
        /* cppcheck-suppress arrayIndexOutOfBoundsCond */
        const char c = s[len];

        if (c == '\0') {
            break;
        }
        if (((uint8_t)c & 0x80U) != 0U) {
            return;
        }
        len++;
    }

    if (!log_try_enqueue(s_core, (const uint8_t *)s, len, NULL, 0U)) {
        log_note_drop(s_core);
    }
}

/* Identifies the IMAGE (LOG_IMAGE_NAME) and, for the security core, the role.
 * Two CM4 images both log as LOG_CORE_APP, so the core alone cannot tell an FBL
 * record from an app record in a capture that spans the jump. */
void log_banner(void)
{
    if (s_core == LOG_CORE_SECURITY) {
        log_text("\n--- " LOG_IMAGE_NAME "/sec " LOG_BUILD_ID " ---\n");
    } else {
        log_text("\n--- " LOG_IMAGE_NAME " " LOG_BUILD_ID " ---\n");
    }
}

/* @impl REQ-LOG-014 : bounded latency; caller runs it at a period no greater
 *                     than LOG_DRAIN_LATENCY_MS.
 * @impl REQ-LOG-004 : walks EVERY core's ring, not just the caller's. */
log_drain_t log_drain(size_t max_bytes)
{
    size_t remaining = max_bytes;
    bool   sent_any  = false;
    size_t core_ix;

    for (core_ix = 0U; core_ix < (size_t)LOG_CORE_COUNT; core_ix++) {
        const log_core_t          core     = (log_core_t)core_ix;
        volatile uint32_t const * const head_cell = log_port_head(core);
        volatile uint32_t * const tail_cell = log_port_tail(core);
        uint8_t * const            ring     = log_port_ring(core);
        const size_t               cap      = log_port_ring_cap(core);
        const size_t               mask     = cap - 1U;

        for (;;) {
            const uint32_t head = *head_cell;
            const uint32_t tail = *tail_cell;
            size_t         used = (size_t)(head - tail);
            size_t         chunk;
            size_t         idx;
            size_t         sent;

            if (used == 0U) {
                break;
            }

            if ((max_bytes != 0U) && (remaining == 0U)) {
                return sent_any ? LOG_DRAIN_SENT : LOG_DRAIN_IDLE;
            }

            if (!log_port_sink_ready()) {
                return LOG_DRAIN_BLOCKED;
            }

            idx   = (size_t)tail & mask;
            chunk = log_min_size(used, cap - idx);
            if ((max_bytes != 0U) && (chunk > remaining)) {
                chunk = remaining;
            }

            sent = log_port_tx(&ring[idx], chunk);
            if (sent == 0U) {
                return LOG_DRAIN_BLOCKED;
            }

            log_port_publish_barrier();
            *tail_cell = tail + (uint32_t)sent;

            sent_any = true;
            if (max_bytes != 0U) {
                remaining -= sent;
            }

            if (sent < chunk) {
                return LOG_DRAIN_BLOCKED;
            }
        }
    }

    return sent_any ? LOG_DRAIN_SENT : LOG_DRAIN_IDLE;
}

/* @impl REQ-LOG-015 : bounded flush at a hand-off (jump / reset).
 *
 * Deliberately the ONE place a caller may wait on the sink, and only because
 * cyclic draining has no next opportunity once the core stops or the UART
 * changes owner. The bound comes from log_port_now_ms(), so a dead sink costs
 * timeout_ms once rather than hanging the hand-off. */
bool log_flush(uint32_t timeout_ms)
{
    const uint32_t start   = log_port_now_ms();
    uint32_t       stalled = 0U;
    bool           drained = false;

    for (;;) {
        bool progress = false;

        /* PHASE 1: empty the rings. */
        if (!drained) {
            const log_drain_t st = log_drain(0U);

            if (st == LOG_DRAIN_IDLE) {
                drained  = true;
                progress = true;
            } else if (st == LOG_DRAIN_SENT) {
                progress = true;
            } else {
                /* BLOCKED: sink busy, no progress this round. */
            }
        }

        /* PHASE 2: an empty ring is not an empty UART. log_port_tx reports bytes
         * accepted into the TX FIFO, not bytes on the wire. Returning here would
         * hand back "flushed" while a record is still shifting out -- and the
         * next SCB re-init (the app's, right after the FBL jumps) resets the
         * FIFO and destroys it. That is exactly the evidence the secure-boot BVT
         * test depends on.
         *
         * Checked BEFORE the deadline so timeout_ms == 0 means "one look", not
         * "fail immediately". */
        if (drained && log_port_sink_idle()) {
            return true;
        }

        /* Bound the spin on ITERATIONS as well as on time: the clock may not be
         * running (the FBL stops SysTick in deinit_for_jump), and a timeout that
         * assumes a live time base is not a bound at all. */
        stalled = progress ? 0U : (stalled + 1U);
        if (stalled >= LOG_FLUSH_MAX_STALL) {
            return false;
        }

        /* Unsigned wrap-safe elapsed comparison: correct across the u32
         * rollover, unlike (now < start + timeout). */
        if ((log_port_now_ms() - start) >= timeout_ms) {
            return false;
        }
    }
}

uint32_t log_dropped(log_core_t core)
{
    return s_dropped[core];
}

/* @impl REQ-LOG-010 : ring bypass + blocking write. The ONE place logging may
 *                     spin, because no drain task will run from a fault. */
void log_panic(uint16_t evt, uint32_t arg0, uint16_t arg1)
{
    uint8_t rec[LOG_REC_SIZE];

    log_rec_encode(rec, s_core, s_seq[s_core], evt, log_port_now_ms(), arg0, arg1);
    log_port_tx_blocking(rec, LOG_REC_SIZE);
}
