/*
 * test_log.c -- contract suite for the target logging channel.
 *
 * Pins ADR-0023 / REQ-LOG-001..014 before the implementation exists. RED until
 * shared/log/src/log.c is filled in (docs/workflow.md handoff).
 *
 * Test harness -- MISRA does not apply (docs/coding-standard.md).
 *
 * What is NOT tested here, deliberately:
 *  - REQ-LOG-001's "no <stdio.h>" is a STATIC property. It is enforced by
 *    make lint + the checks/ layer rules, not by a runtime assertion.
 *  - REQ-LOG-004's non-cacheable PLACEMENT is a linker/MPU property, provable
 *    only on target (@design-only). The ring ALGORITHM is tested here.
 *  - REQ-LOG-013's C-vs-Python equivalence lives in test_log_decode (host
 *    tool side); this file pins the C encoder it compares against.
 */
#include "unity.h"
#include "tb_log.h"
#include "log_port.h"
#include "log_port_fake.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------ */

/* Read a little-endian field out of an encoded record. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Drain everything the sink will take, then forget it -- gives each test a
 * clean stream without fighting the banner that log_init() emits. */
static void drain_all_and_forget(void)
{
    (void)log_drain(0U);
    log_fake_reset_sink_only();
}

void setUp(void)
{
    log_fake_reset();
    log_init(LOG_CORE_APP);
    drain_all_and_forget();
}

void tearDown(void)
{
    /* Every test must leave the critical section balanced -- a missing unlock
     * on target means interrupts stay masked forever (REQ-LOG-005). */
    TEST_ASSERT_TRUE_MESSAGE(log_fake_lock_balanced(), "log_port_lock/unlock unbalanced");
}

/* ---- REQ-LOG-002: record layout ----------------------------------- */

/* @test REQ-LOG-002 : CRC-16/CCITT-FALSE check value */
void test_crc16_matches_the_standard_check_value(void)
{
    /* The canonical CCITT-FALSE check: CRC("123456789") == 0x29B1. If this
     * fails, the C and Python sides will silently disagree (REQ-LOG-013). */
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1U, log_crc16(v, 9U));
}

/* @test REQ-LOG-002 : explicit byte offsets, little-endian, 16 bytes */
void test_record_layout_is_byte_exact(void)
{
    uint8_t r[LOG_REC_SIZE];

    memset(r, 0xEE, sizeof r);
    log_rec_encode(r, LOG_CORE_SECURITY, 0x05U, 0x0102U, 0xDEADBEEFU, 0xCAFEF00DU, 0x1234U);

    TEST_ASSERT_EQUAL_HEX8(LOG_SYNC, r[LOG_OFF_SYNC]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((LOG_CORE_SECURITY << LOG_CORE_SHIFT) | 0x05U),
                           r[LOG_OFF_CORE_SEQ]);
    TEST_ASSERT_EQUAL_HEX16(0x0102U,     rd16(&r[LOG_OFF_EVT]));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, rd32(&r[LOG_OFF_TS_MS]));
    TEST_ASSERT_EQUAL_HEX32(0xCAFEF00DU, rd32(&r[LOG_OFF_ARG0]));
    TEST_ASSERT_EQUAL_HEX16(0x1234U,     rd16(&r[LOG_OFF_ARG1]));
    TEST_ASSERT_EQUAL_HEX16(log_crc16(r, LOG_OFF_CRC), rd16(&r[LOG_OFF_CRC]));
}

/* @test REQ-LOG-003 : sync is outside ASCII, so text can share the stream */
void test_sync_byte_is_outside_ascii(void)
{
    TEST_ASSERT_TRUE_MESSAGE(LOG_SYNC >= 0x80U,
                             "LOG_SYNC must be >= 0x80 or text passthrough forges records");
}

/* @test REQ-LOG-002 : seq occupies 6 bits, core the top 2 */
void test_core_and_seq_pack_into_one_byte(void)
{
    uint8_t r[LOG_REC_SIZE];

    /* seq must be masked, not allowed to bleed into the core field. */
    log_rec_encode(r, LOG_CORE_APP, 0xFFU, 0U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8(LOG_SEQ_MASK, (uint8_t)(r[LOG_OFF_CORE_SEQ] & LOG_SEQ_MASK));
    TEST_ASSERT_EQUAL_UINT8(LOG_CORE_APP,
                            (uint8_t)((r[LOG_OFF_CORE_SEQ] >> LOG_CORE_SHIFT) & LOG_CORE_MASK));
}

/* ---- REQ-LOG-005: the producer never blocks ----------------------- */

/* @test REQ-LOG-005 : log_evt() enqueues only -- it never touches the sink */
void test_log_evt_does_not_write_to_the_sink(void)
{
    log_evt(0x0401U, 42U, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_ring_used());
}

/* @test REQ-LOG-005 : one enqueue takes the lock exactly once, never nested.
 *
 * Measured as a DELTA, not an absolute: log_init() emits the banner, which is
 * itself an enqueue and takes the lock. Asserting an absolute count would make
 * this test depend on how many times setUp happened to log -- a coupling that
 * breaks the moment the banner changes. */
void test_critical_section_is_entered_once_and_not_nested(void)
{
    const uint32_t before = log_fake_lock_enter_count();

    log_evt(0x0401U, 1U, 0U);

    TEST_ASSERT_EQUAL_UINT32(1U, log_fake_lock_enter_count() - before);
    TEST_ASSERT_EQUAL_UINT32(1U, log_fake_lock_max_depth());
}

/* @test REQ-LOG-005 : head/tail handoff is published explicitly, not assumed */
void test_publish_barrier_is_issued(void)
{
    log_evt(0x0401U, 1U, 0U);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, log_fake_barrier_count());
}

/* ---- ADR-0023 D10: timestamps are sampled at PRODUCTION time ------- */

/* @test ADR-0023 D10 : cyclic drain must not distort the recorded timeline.
 * This is the property that makes buffered output cost freshness, not truth --
 * if it ever breaks, every timing claim read off a log becomes wrong. */
void test_timestamp_is_sampled_when_logged_not_when_drained(void)
{
    log_fake_set_now(100U);
    log_evt(0x0401U, 0U, 0U);

    log_fake_set_now(500U);          /* time passes before the drain task runs */
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT32(100U, rd32(&log_fake_sink()[LOG_OFF_TS_MS]));
}

/* ---- REQ-LOG-006: overflow is bounded, counted, announced --------- */

/* @test REQ-LOG-006 : a full ring drops the NEWEST and counts it */
void test_overflow_drops_newest_and_counts(void)
{
    const uint32_t fits = (uint32_t)(FAKE_RING_CAP / LOG_REC_SIZE);
    uint32_t i;

    for (i = 0U; i < fits; i++) {
        log_evt(0x0401U, i, 0U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, log_dropped());

    log_evt(0x0401U, 0xFFFFU, 0U);   /* one too many */
    TEST_ASSERT_EQUAL_UINT32(1U, log_dropped());
}

/* @test REQ-LOG-006 : the OLDEST records survive -- they explain the failure */
void test_overflow_preserves_the_earliest_records(void)
{
    const uint32_t fits = (uint32_t)(FAKE_RING_CAP / LOG_REC_SIZE);
    uint32_t i;

    for (i = 0U; i < (fits + 3U); i++) {
        log_evt(0x0401U, i, 0U);
    }
    (void)log_drain(0U);

    /* Assert the stream is non-empty BEFORE reading a field out of it --
     * otherwise an empty sink reads back as zeros and this test passes
     * vacuously, which is the exact false-pass REQ-LOG-007 exists to forbid. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(LOG_REC_SIZE, log_fake_sink_len());

    /* First record out is arg0 == 0: drop-newest never advances the tail. */
    TEST_ASSERT_EQUAL_UINT32(0U, rd32(&log_fake_sink()[LOG_OFF_ARG0]));
}

/* @test REQ-LOG-006 : loss is announced once space frees */
void test_overflow_event_is_emitted_with_the_lost_count(void)
{
    const uint32_t fits = (uint32_t)(FAKE_RING_CAP / LOG_REC_SIZE);
    uint32_t i;
    size_t   n;
    bool     found = false;

    for (i = 0U; i < (fits + 2U); i++) {
        log_evt(0x0401U, i, 0U);
    }
    TEST_ASSERT_EQUAL_UINT32(2U, log_dropped());

    (void)log_drain(0U);             /* space frees */
    log_evt(0x0401U, 999U, 0U);      /* next write reports the loss */
    (void)log_drain(0U);

    for (n = 0U; (n + LOG_REC_SIZE) <= log_fake_sink_len(); n += LOG_REC_SIZE) {
        if (rd16(&log_fake_sink()[n + LOG_OFF_EVT]) == 0x0004U) {   /* LOG_EVT_OVERFLOW */
            TEST_ASSERT_EQUAL_UINT32(2U, rd32(&log_fake_sink()[n + LOG_OFF_ARG0]));
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "LOG_EVT_OVERFLOW never reported the drop");
}

/* @test REQ-LOG-006 : seq lets a consumer detect loss INDEPENDENTLY, so a lost
 * overflow record cannot hide the loss. This is what stops the BVT from
 * reading "event absent" when the truth is "event dropped". */
void test_sequence_number_advances_per_record_and_wraps(void)
{
    uint8_t first;
    uint8_t second;

    log_evt(0x0401U, 0U, 0U);
    log_evt(0x0401U, 1U, 0U);
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(2U * LOG_REC_SIZE, log_fake_sink_len());
    first  = (uint8_t)(log_fake_sink()[LOG_OFF_CORE_SEQ] & LOG_SEQ_MASK);
    second = (uint8_t)(log_fake_sink()[LOG_REC_SIZE + LOG_OFF_CORE_SEQ] & LOG_SEQ_MASK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((first + 1U) & LOG_SEQ_MASK), second);
}

/* ---- ring mechanics ----------------------------------------------- */

/* @test REQ-LOG-004 : a record straddling the ring wrap survives intact */
void test_record_spanning_the_ring_wrap_is_intact(void)
{
    const uint32_t fits = (uint32_t)(FAKE_RING_CAP / LOG_REC_SIZE);
    uint32_t i;

    /* Fill, drain, then write again so the next reservation wraps. */
    for (i = 0U; i < fits; i++) {
        log_evt(0x0401U, i, 0U);
    }
    (void)log_drain(0U);
    log_fake_reset_sink_only();

    log_evt(0x0401U, 0xA1B2C3D4U, 0x5566U);
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_sink_len());
    TEST_ASSERT_EQUAL_HEX8(LOG_SYNC, log_fake_sink()[LOG_OFF_SYNC]);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4U, rd32(&log_fake_sink()[LOG_OFF_ARG0]));
    TEST_ASSERT_EQUAL_HEX16(0x5566U,     rd16(&log_fake_sink()[LOG_OFF_ARG1]));
}

/* ---- REQ-LOG-014: drain behaviour --------------------------------- */

/* @test REQ-LOG-014 : a busy sink leaves bytes queued and says so */
void test_drain_reports_blocked_and_keeps_the_remainder(void)
{
    log_drain_t st;

    log_evt(0x0401U, 1U, 0U);
    log_fake_set_tx_accept(4U);      /* UART takes 4 of the 16 bytes */

    st = log_drain(0U);
    TEST_ASSERT_EQUAL(LOG_DRAIN_BLOCKED, st);
    TEST_ASSERT_EQUAL_UINT(4U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE - 4U, log_fake_ring_used());
}

/* @test REQ-LOG-014 : max_bytes bounds one call so drain cannot hog the core */
void test_drain_respects_max_bytes(void)
{
    log_evt(0x0401U, 1U, 0U);
    log_evt(0x0401U, 2U, 0U);

    (void)log_drain(LOG_REC_SIZE);
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_sink_len());
}

/* @test REQ-LOG-014 : nothing queued is not an error */
void test_drain_on_empty_ring_is_idle(void)
{
    TEST_ASSERT_EQUAL(LOG_DRAIN_IDLE, log_drain(0U));
    TEST_ASSERT_EQUAL_UINT(0U, log_fake_sink_len());
}

/* @test REQ-LOG-005 : an unready sink must not lose the producer's records --
 * they queue, and the boot banner survives (drop-newest, REQ-LOG-006). */
void test_records_queue_while_the_sink_is_not_ready(void)
{
    log_fake_set_sink_ready(false);
    log_evt(0x0401U, 7U, 0U);
    (void)log_drain(0U);
    TEST_ASSERT_EQUAL_UINT(0U, log_fake_sink_len());

    log_fake_set_sink_ready(true);
    (void)log_drain(0U);
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT32(7U, rd32(&log_fake_sink()[LOG_OFF_ARG0]));
}

/* ---- REQ-LOG-015: bounded flush at a hand-off --------------------- */

/* @test REQ-LOG-015 : flush empties the rings and reports success */
void test_flush_drains_everything(void)
{
    log_evt(0x0002U, 0x10040000U, 0U);
    log_evt(0x0401U, 1U, 0U);

    TEST_ASSERT_TRUE(log_flush(100U));
    TEST_ASSERT_EQUAL_UINT(2U * LOG_REC_SIZE, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT(0U, log_fake_ring_used());
}

/* @test REQ-LOG-015 : a dead sink AND a FROZEN CLOCK must still terminate.
 *
 * The scenario is real, not hypothetical: the FBL calls log_flush() on the way
 * to the app, and fbl_port_deinit_for_jump() stops SysTick — so
 * log_port_now_ms() can be frozen at exactly the moment flush runs. A timeout
 * that assumes a live time base is not a bound at all, and the logger would
 * hang the boot path it exists to observe. The clock is left at its default
 * (step 0 = frozen) precisely to pin that case; termination comes from
 * LOG_FLUSH_MAX_STALL, not from time. */
void test_flush_terminates_with_a_dead_sink_and_frozen_clock(void)
{
    log_evt(0x0401U, 1U, 0U);
    log_fake_set_sink_ready(false);

    TEST_ASSERT_FALSE(log_flush(10U));
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_ring_used());
}

/* @test REQ-LOG-015 : with a running clock, the timeout is what bounds it */
void test_flush_times_out_when_the_clock_runs(void)
{
    log_evt(0x0401U, 1U, 0U);
    log_fake_set_sink_ready(false);
    log_fake_set_now_step(1U);       /* 1 ms per now_ms() call */

    TEST_ASSERT_FALSE(log_flush(5U));
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_ring_used());
}

/* @test REQ-LOG-015 : nothing queued is a successful flush, not a timeout */
void test_flush_on_empty_rings_succeeds_immediately(void)
{
    TEST_ASSERT_TRUE(log_flush(0U));
}

/* @test REQ-LOG-015 : an empty RING is not an empty UART.
 *
 * This is the bug that lost every FBL record on real hardware. log_port_tx()
 * reports bytes accepted into the TX FIFO, so the ring empties long before the
 * bytes reach the wire -- ~160 us for one record at 1 Mbps, against a jump that
 * takes microseconds. Whoever re-inits the SCB next (the app, right after the
 * FBL jumps) resets the FIFO and the evidence is gone. Flush must wait for the
 * sink to go idle, not merely for the ring to drain. */
void test_flush_waits_for_the_sink_to_go_idle(void)
{
    log_evt(0x0002U, 0x10040000U, 0U);
    log_fake_set_sink_idle(false);      /* bytes accepted, still shifting out */
    log_fake_set_now_step(1U);

    TEST_ASSERT_FALSE_MESSAGE(log_flush(5U),
        "flush reported success while the UART was still transmitting");
    TEST_ASSERT_EQUAL_UINT(0U, log_fake_ring_used());   /* ring DID drain */

    /* Once the shifter empties, the same flush succeeds. */
    log_fake_set_sink_idle(true);
    TEST_ASSERT_TRUE(log_flush(5U));
}

/* ---- REQ-LOG-003: text passthrough -------------------------------- */

/* @test REQ-LOG-003 : ASCII passes through verbatim, no framing added */
void test_log_text_passes_ascii_through_unescaped(void)
{
    log_text("hi\n");
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(3U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_MEMORY("hi\n", log_fake_sink(), 3U);
}

/* @test REQ-LOG-003 : a high-bit byte would forge LOG_SYNC -- reject the run.
 *
 * Bracketed by a VALID write so a do-nothing implementation cannot pass: the
 * assertion is "ok survived and the bad run did not", not the weaker (and
 * vacuously true) "the sink is empty". */
void test_log_text_rejects_non_ascii(void)
{
    log_text("ok");
    log_text("bad\xA5""x");
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(2U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_MEMORY("ok", log_fake_sink(), 2U);
}

/* @test REQ-LOG-005 : text is truncated at LOG_TEXT_MAX.
 *
 * This bounds the interrupt-masked region, which now spans the copy (ADR-0023
 * D7). An unbounded log_text() would mask interrupts for as long as the string
 * -- so this is a concurrency guarantee wearing a string-length costume. */
void test_log_text_is_truncated_at_the_bound(void)
{
    char big[LOG_TEXT_MAX + 20U];

    memset(big, 'x', sizeof big);
    big[sizeof big - 1U] = '\0';

    log_text(big);
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(LOG_TEXT_MAX, log_fake_sink_len());
}

/* @test REQ-LOG-005 : head must never move backwards.
 *
 * The publish step runs under the lock so reserve+copy+publish is indivisible
 * (see log_try_enqueue). A single-threaded host test cannot stage the
 * task/ISR interleaving that breaks it, but it CAN pin the invariant that
 * interleaving would violate: head is monotonic across every enqueue. If a
 * future change reintroduces publish-outside-the-lock, head regression is what
 * shows up first -- and it underflows `head - tail`, which floods the sink. */
void test_head_advances_monotonically(void)
{
    uint32_t prev = *log_port_head();
    uint32_t i;

    for (i = 0U; i < 8U; i++) {
        uint32_t now;
        log_evt(0x0401U, i, 0U);
        now = *log_port_head();
        TEST_ASSERT_TRUE_MESSAGE(now >= prev, "ring head moved backwards");
        prev = now;
        (void)log_drain(0U);
    }
}

/* @test REQ-LOG-003 : NULL is a no-op, not a fault -- and not a stall.
 *
 * Same reasoning: writes on either side prove the channel still works, so a
 * stub that ignores every call fails rather than passes. */
void test_log_text_tolerates_null(void)
{
    log_text("a");
    log_text(NULL);
    log_text("b");
    (void)log_drain(0U);

    TEST_ASSERT_EQUAL_UINT(2U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_MEMORY("ab", log_fake_sink(), 2U);
}

/* ---- REQ-LOG-010: the panic path ---------------------------------- */

/* @test REQ-LOG-010 : log_panic() bypasses the ring entirely */
void test_log_panic_bypasses_the_ring(void)
{
    log_panic(0x0003U, 0x20001000U, 0x0007U);

    TEST_ASSERT_EQUAL_UINT(0U, log_fake_ring_used());
    TEST_ASSERT_EQUAL_UINT(0U, log_fake_sink_len());
    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_panic_sink_len());
    TEST_ASSERT_EQUAL_HEX8(LOG_SYNC, log_fake_panic_sink()[LOG_OFF_SYNC]);
    TEST_ASSERT_EQUAL_HEX16(0x0003U, rd16(&log_fake_panic_sink()[LOG_OFF_EVT]));
}

/* @test REQ-LOG-010 : it works when the ring is full -- that is the point.
 * A fault handler must be able to speak precisely when the system is wedged. */
void test_log_panic_works_with_a_full_ring(void)
{
    const uint32_t fits = (uint32_t)(FAKE_RING_CAP / LOG_REC_SIZE);
    uint32_t i;

    for (i = 0U; i < (fits + 5U); i++) {
        log_evt(0x0401U, i, 0U);
    }
    log_panic(0x0003U, 0xBADC0DEU, 0U);

    TEST_ASSERT_EQUAL_UINT(LOG_REC_SIZE, log_fake_panic_sink_len());
    TEST_ASSERT_EQUAL_HEX32(0xBADC0DEU, rd32(&log_fake_panic_sink()[LOG_OFF_ARG0]));
}

/* ---- REQ-LOG-003: the banner -------------------------------------- */

/* @test REQ-LOG-003 : the banner is ASCII so a plain terminal can read it
 * before any decoder exists -- which is exactly when you need evidence. */
void test_banner_is_plain_ascii(void)
{
    size_t i;

    log_banner();
    (void)log_drain(0U);

    TEST_ASSERT_GREATER_THAN_UINT(0U, log_fake_sink_len());
    for (i = 0U; i < log_fake_sink_len(); i++) {
        TEST_ASSERT_TRUE_MESSAGE(log_fake_sink()[i] < 0x80U, "banner must be ASCII");
    }
}

/* ---- runner ------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_crc16_matches_the_standard_check_value);
    RUN_TEST(test_record_layout_is_byte_exact);
    RUN_TEST(test_sync_byte_is_outside_ascii);
    RUN_TEST(test_core_and_seq_pack_into_one_byte);

    RUN_TEST(test_log_evt_does_not_write_to_the_sink);
    RUN_TEST(test_critical_section_is_entered_once_and_not_nested);
    RUN_TEST(test_publish_barrier_is_issued);
    RUN_TEST(test_timestamp_is_sampled_when_logged_not_when_drained);

    RUN_TEST(test_overflow_drops_newest_and_counts);
    RUN_TEST(test_overflow_preserves_the_earliest_records);
    RUN_TEST(test_overflow_event_is_emitted_with_the_lost_count);
    RUN_TEST(test_sequence_number_advances_per_record_and_wraps);

    RUN_TEST(test_record_spanning_the_ring_wrap_is_intact);

    RUN_TEST(test_drain_reports_blocked_and_keeps_the_remainder);
    RUN_TEST(test_drain_respects_max_bytes);
    RUN_TEST(test_drain_on_empty_ring_is_idle);
    RUN_TEST(test_records_queue_while_the_sink_is_not_ready);

    RUN_TEST(test_flush_drains_everything);
    RUN_TEST(test_flush_terminates_with_a_dead_sink_and_frozen_clock);
    RUN_TEST(test_flush_times_out_when_the_clock_runs);
    RUN_TEST(test_flush_on_empty_rings_succeeds_immediately);
    RUN_TEST(test_flush_waits_for_the_sink_to_go_idle);

    RUN_TEST(test_log_text_passes_ascii_through_unescaped);
    RUN_TEST(test_log_text_rejects_non_ascii);
    RUN_TEST(test_log_text_is_truncated_at_the_bound);
    RUN_TEST(test_head_advances_monotonically);
    RUN_TEST(test_log_text_tolerates_null);

    RUN_TEST(test_log_panic_bypasses_the_ring);
    RUN_TEST(test_log_panic_works_with_a_full_ring);

    RUN_TEST(test_banner_is_plain_ascii);

    return UNITY_END();
}
