/*
 * test_secoc_rx.c — the SecOC receive verdict seam (ADR-0023 D11 / REQ-LOG-009,
 * REQ-SECOC-001, REQ-SECOC-007, REQ-SECOC-008). RED until secoc_rx.c exists.
 *
 * This suite is the reason the logging channel was built. The BVT's replay and
 * forgery tests assert that the actuator HELD its last-known-good state — a
 * negative observation that passes identically if the node is dead, wedged, or
 * never listening. The reject events turn that into a positive assertion, so
 * these events ARE the evidence, and evidence gets tested like anything else.
 *
 * The load-bearing properties, in order of weight:
 *   1. Counters and events cannot diverge (REQ-LOG-009's actual words).
 *   2. A dead oracle is distinguishable from a forgery. Same verdict, different
 *      reason — without that, a forgery test goes green when the M0+ is dead,
 *      which is a false pass on the security path.
 *   3. Exactly one event per frame. This is a per-frame path (ADR-0023 D7).
 *   4. A rejected frame yields no PDU, whatever the caller does with the verdict.
 *
 * Test harness — MISRA does not apply (docs/coding-standard.md).
 */
#include "unity.h"
#include "secoc_rx.h"
#include "secoc_mac_fake.h"
#include "secoc_freshness_store_fake.h"
#include "tb_log.h"
#include "log_events.h"
#include "log_types.h"
#include "log_port_fake.h"

#include <string.h>

#define ID_DOOR    0x120U
#define ID_LIGHT   0x121U
#define ID_SYNC    0x2F0U
#define ID_UNKNOWN 0x321U        /* deliberately NOT registered on this receiver */

#define DOM_RX     1U
#define DOM_TX     0U

static const secoc_crypto_t g_cy  = { &g_secoc_mac_fake, SECOC_FAKE_KEY_ID };
static const secoc_crypto_t g_cy0 = { &g_secoc_mac_fake, 0U };   /* oracle refuses key 0 */

static const uint16_t g_rx_ids[] = { ID_DOOR, ID_LIGHT };

static secoc_rx_ctx_t g_fresh;
static secoc_rx_t     g_rx;

/* ---- record readback ---------------------------------------------- */

typedef struct { uint16_t evt; uint32_t arg0; uint16_t arg1; } rec_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Drain the ring and hand back the records produced since the last call.
 * Reads only whole 16-byte records; the banner is discarded in setUp, so
 * anything here was produced by the code under test. */
static size_t take_records(rec_t *out, size_t max)
{
    (void)log_drain(0U);
    const uint8_t *b = log_fake_sink();
    size_t n = log_fake_sink_len() / LOG_REC_SIZE;
    if (n > max) { n = max; }
    for (size_t i = 0U; i < n; ++i)
    {
        const uint8_t *r = &b[i * LOG_REC_SIZE];
        out[i].evt  = rd16(&r[LOG_OFF_EVT]);
        out[i].arg0 = rd32(&r[LOG_OFF_ARG0]);
        out[i].arg1 = rd16(&r[LOG_OFF_ARG1]);
    }
    log_fake_reset_sink_only();
    return n;
}

/* The common case: assert one event was produced, and return it. */
static rec_t take_one(void)
{
    rec_t r[8];
    size_t n = take_records(r, 8U);
    TEST_ASSERT_EQUAL_MESSAGE(1, n, "expected exactly one event for this frame");
    return r[0];
}

/* ---- fixtures ------------------------------------------------------ */

/* Build a valid secured frame for `can_id` at (epoch, counter). */
static size_t make_frame(uint16_t can_id, uint16_t epoch, uint16_t counter,
                         const uint8_t *pdu, size_t pdu_len, uint8_t *out, size_t cap)
{
    return secoc_secure(&g_cy, can_id, epoch, counter, pdu, pdu_len, out, cap);
}

void setUp(void)
{
    log_fake_reset();
    log_init(LOG_CORE_APP);
    (void)log_drain(0U);
    log_fake_reset_sink_only();          /* discard the boot banner */

    secoc_store_fake_reset();
    TEST_ASSERT_TRUE(secoc_rx_boot(&g_fresh, &g_secoc_store_fake, (uint8_t)DOM_RX,
                                   g_rx_ids, sizeof g_rx_ids / sizeof g_rx_ids[0]));
    secoc_rx_init(&g_rx, &g_cy, &g_fresh);
}

void tearDown(void)
{
    /* An unbalanced mask on target means interrupts stay off forever — and this
     * is the path that runs per received frame (REQ-LOG-005). */
    TEST_ASSERT_TRUE_MESSAGE(log_fake_lock_balanced(), "log_port_lock/unlock unbalanced");
}

/* ---- accept -------------------------------------------------------- */

/* @test REQ-LOG-009 : accept emits LOG_EVT_SECOC_ACCEPT(can_id, freshness_ctr) */
/* @test REQ-SECOC-001 : an authentic, fresh frame is released to decode */
void test_accept_emits_accept_event_with_id_and_counter(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 7U, pdu, sizeof pdu, frame, sizeof frame);

    uint8_t out[16]; size_t out_len = 0U;
    TEST_ASSERT_EQUAL(SECOC_RX_ACCEPT,
        secoc_rx_process(&g_rx, ID_DOOR, frame, n, out, sizeof out, &out_len));

    TEST_ASSERT_EQUAL_size_t(sizeof pdu, out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pdu, out, sizeof pdu);

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_ACCEPT, r.evt);
    TEST_ASSERT_EQUAL_HEX32(ID_DOOR, r.arg0);
    TEST_ASSERT_EQUAL_UINT16(7U, r.arg1);
    TEST_ASSERT_EQUAL_UINT32(1U, g_rx.n_accept);
}

/* ---- reject: MAC, and the three sub-reasons ------------------------ */

/* @test REQ-LOG-009 : a forged MAC emits REJECT_MAC with reason SECOC_BAD_MAC */
/* @test REQ-SECOC-001 : bad MAC ⇒ drop before decode */
void test_forged_mac_emits_reject_mac_reason_bad_mac(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 7U, pdu, sizeof pdu, frame, sizeof frame);
    frame[n - 1U] ^= 0xFFU;                          /* flip a MAC byte */

    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&g_rx, ID_DOOR, frame, n, out, sizeof out, &out_len));

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_MAC, r.evt);
    TEST_ASSERT_EQUAL_HEX32(ID_DOOR, r.arg0);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)SECOC_BAD_MAC, r.arg1);
    TEST_ASSERT_EQUAL_UINT32(1U, g_rx.n_drop_mac);
}

/* @test REQ-LOG-009 : a malformed frame is reason SECOC_BAD_LENGTH, not a forgery */
void test_short_frame_emits_reject_mac_reason_bad_length(void)
{
    uint8_t frame[8] = { 0U };                       /* shorter than the 12-byte trailer */
    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&g_rx, ID_DOOR, frame, sizeof frame, out, sizeof out, &out_len));

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_MAC, r.evt);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)SECOC_BAD_LENGTH, r.arg1);
}

/* THE false-pass guard. A dead M0+ rejects every frame — including a forged one
 * — so a forgery test that asserts only "rejected" passes on a broken node.
 * arg1 is what tells the two apart, so it is a contract, not decoration.
 *
 * @test REQ-LOG-009 : oracle failure is reason SECOC_MAC_ERROR, distinct from a forgery
 * @test REQ-SECOC-001 : unknown key ⇒ fail-safe drop */
void test_oracle_failure_is_distinguishable_from_a_forgery(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 7U, pdu, sizeof pdu, frame, sizeof frame);

    secoc_rx_t dead;
    secoc_rx_init(&dead, &g_cy0, &g_fresh);          /* key 0 ⇒ the fake oracle refuses */

    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&dead, ID_DOOR, frame, n, out, sizeof out, &out_len));

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_MAC, r.evt);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)SECOC_MAC_ERROR, r.arg1);
    TEST_ASSERT_NOT_EQUAL_MESSAGE((uint16_t)SECOC_BAD_MAC, r.arg1,
        "a dead oracle must not look like a rejected forgery");
}

/* @test REQ-SECOC-001 : misuse fails safe and stays observable */
void test_unbound_receiver_drops_and_still_reports(void)
{
    secoc_rx_t unbound;
    secoc_rx_init(&unbound, NULL, NULL);

    uint8_t frame[32] = { 0U };
    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&unbound, ID_DOOR, frame, sizeof frame, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(0U, out_len);

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_MAC, r.evt);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)SECOC_MAC_ERROR, r.arg1);
}

/* ---- reject: freshness --------------------------------------------- */

/* @test REQ-LOG-009 : a replay emits REJECT_FRESHNESS carrying the received counter */
/* @test REQ-SECOC-007 : the same (epoch,counter) is not accepted twice */
void test_replayed_frame_emits_reject_freshness_with_rx_counter(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 42U, pdu, sizeof pdu, frame, sizeof frame);

    uint8_t out[16]; size_t out_len = 0U;
    TEST_ASSERT_EQUAL(SECOC_RX_ACCEPT,
        secoc_rx_process(&g_rx, ID_DOOR, frame, n, out, sizeof out, &out_len));
    (void)take_one();                                 /* the ACCEPT */

    out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_FRESHNESS,
        secoc_rx_process(&g_rx, ID_DOOR, frame, n, out, sizeof out, &out_len));

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_FRESHNESS, r.evt);
    TEST_ASSERT_EQUAL_HEX32(ID_DOOR, r.arg0);
    TEST_ASSERT_EQUAL_UINT16(42U, r.arg1);
    TEST_ASSERT_EQUAL_UINT32(1U, g_rx.n_drop_fresh);
    TEST_ASSERT_EQUAL_UINT32(1U, g_rx.n_accept);      /* the replay did NOT count as one */
}

/* An authentic frame on an ID this receiver does not protect is a configuration
 * fault, not a forgery — the MAC verified. It counts as a freshness drop, which
 * is where the existing per-reason counter put it, and it must still be visible.
 *
 * @test REQ-SECOC-007 : an unregistered CAN ID is never accepted */
void test_unregistered_can_id_is_a_freshness_drop(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_UNKNOWN, g_fresh.floor, 1U, pdu, sizeof pdu, frame, sizeof frame);

    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_FRESHNESS,
        secoc_rx_process(&g_rx, ID_UNKNOWN, frame, n, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(0U, out_len);

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_FRESHNESS, r.evt);
    TEST_ASSERT_EQUAL_HEX32(ID_UNKNOWN, r.arg0);
}

/* ---- drop-before-decode -------------------------------------------- */

/* A caller that ignores the verdict still must not see plaintext. The event and
 * the empty PDU are the same guarantee stated twice, on purpose.
 *
 * @test ADR-0021 D9 : a rejected frame yields no PDU */
void test_rejected_frame_yields_no_pdu(void)
{
    const uint8_t pdu[3] = { 0xDEU, 0xADU, 0xBEU };
    uint8_t frame[32];
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 5U, pdu, sizeof pdu, frame, sizeof frame);
    frame[0] ^= 0xFFU;                                /* tamper the PDU ⇒ MAC fails */

    uint8_t out[16];
    memset(out, 0x5AU, sizeof out);
    size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&g_rx, ID_DOOR, frame, n, out, sizeof out, &out_len));

    TEST_ASSERT_EQUAL_size_t(0U, out_len);
    for (size_t i = 0U; i < sizeof out; ++i)
    {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5AU, out[i], "rejected frame leaked bytes into pdu_out");
    }
    (void)take_one();
}

/* ---- the REQ-LOG-009 property itself ------------------------------- */

/* The requirement's actual words: the events "are the observable form of the
 * per-reason drop counters ... the two shall not diverge". Run a mixed traffic
 * pattern and assert the tally the host sees equals the tally the node kept.
 *
 * @test REQ-LOG-009 : events and per-reason counters agree
 * @test REQ-SECOC-001 : every drop is counted by reason */
void test_events_and_counters_never_diverge(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t good[32], forged[32], stale[32];
    uint8_t out[16]; size_t out_len = 0U;
    uint16_t e0 = g_fresh.floor;

    size_t ng = make_frame(ID_DOOR,  e0, 10U, pdu, sizeof pdu, good,   sizeof good);
    size_t nf = make_frame(ID_LIGHT, e0, 11U, pdu, sizeof pdu, forged, sizeof forged);
    forged[nf - 1U] ^= 0x01U;
    size_t ns = make_frame(ID_DOOR,  e0,  9U, pdu, sizeof pdu, stale,  sizeof stale);

    (void)secoc_rx_process(&g_rx, ID_DOOR,  good,   ng, out, sizeof out, &out_len);
    (void)secoc_rx_process(&g_rx, ID_LIGHT, forged, nf, out, sizeof out, &out_len);
    (void)secoc_rx_process(&g_rx, ID_DOOR,  stale,  ns, out, sizeof out, &out_len);  /* ≤ high-water */
    (void)secoc_rx_process(&g_rx, ID_DOOR,  good,   ng, out, sizeof out, &out_len);  /* replay */

    rec_t r[16];
    size_t n = take_records(r, 16U);
    TEST_ASSERT_EQUAL_MESSAGE(4, n, "one event per frame, no more and no fewer");

    uint32_t seen_accept = 0U, seen_mac = 0U, seen_fresh = 0U;
    for (size_t i = 0U; i < n; ++i)
    {
        switch (r[i].evt)
        {
        case LOG_EVT_SECOC_ACCEPT:            ++seen_accept; break;
        case LOG_EVT_SECOC_REJECT_MAC:        ++seen_mac;    break;
        case LOG_EVT_SECOC_REJECT_FRESHNESS:  ++seen_fresh;  break;
        default: TEST_FAIL_MESSAGE("unexpected event on the SecOC verify path"); break;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(g_rx.n_accept,     seen_accept);
    TEST_ASSERT_EQUAL_UINT32(g_rx.n_drop_mac,   seen_mac);
    TEST_ASSERT_EQUAL_UINT32(g_rx.n_drop_fresh, seen_fresh);

    TEST_ASSERT_EQUAL_UINT32(1U, seen_accept);
    TEST_ASSERT_EQUAL_UINT32(1U, seen_mac);
    TEST_ASSERT_EQUAL_UINT32(2U, seen_fresh);
}

/* ---- resync (Node A side, ADR-0021 D5) ----------------------------- */

/* @test REQ-LOG-009 : resync emits LOG_EVT_SECOC_RESYNC(can_id, new_epoch) */
/* @test REQ-SECOC-008 : the sender adopts an authenticated floor */
void test_sync_adopt_emits_resync_with_the_new_epoch(void)
{
    static const uint16_t tx_ids[] = { ID_DOOR, ID_LIGHT };
    secoc_tx_ctx_t tx;
    rec_t boot_recs[4];
    TEST_ASSERT_TRUE(secoc_tx_boot(&tx, &g_secoc_store_fake, (uint8_t)DOM_TX,
                                   tx_ids, sizeof tx_ids / sizeof tx_ids[0]));
    TEST_ASSERT_EQUAL_MESSAGE(0, take_records(boot_recs, 4U),
                              "booting a sender is not a SecOC verdict");

    uint16_t floor = (uint16_t)(tx.epoch + 5U);
    TEST_ASSERT_TRUE(secoc_rx_sync_adopt(&tx, ID_SYNC, floor));

    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_RESYNC, r.evt);
    TEST_ASSERT_EQUAL_HEX32(ID_SYNC, r.arg0);
    TEST_ASSERT_EQUAL_UINT16(floor, r.arg1);
    TEST_ASSERT_EQUAL_UINT16(floor, tx.epoch);
}

/* A repeated sync carrying a floor already covered is not a resync completing.
 * Emitting there would let the BVT assert a recovery that never happened.
 *
 * @test REQ-LOG-009 : no event when the epoch does not move */
void test_sync_adopt_is_silent_when_the_epoch_does_not_move(void)
{
    static const uint16_t tx_ids[] = { ID_DOOR };
    secoc_tx_ctx_t tx;
    TEST_ASSERT_TRUE(secoc_tx_boot(&tx, &g_secoc_store_fake, (uint8_t)DOM_TX,
                                   tx_ids, sizeof tx_ids / sizeof tx_ids[0]));
    uint16_t floor = (uint16_t)(tx.epoch + 3U);
    TEST_ASSERT_TRUE(secoc_rx_sync_adopt(&tx, ID_SYNC, floor));
    (void)take_one();

    TEST_ASSERT_FALSE(secoc_rx_sync_adopt(&tx, ID_SYNC, floor));          /* duplicate */
    TEST_ASSERT_FALSE(secoc_rx_sync_adopt(&tx, ID_SYNC, (uint16_t)(floor - 1U)));  /* backwards */

    rec_t r[4];
    TEST_ASSERT_EQUAL_MESSAGE(0, take_records(r, 4U),
                              "a floor that changes nothing must not report a resync");
}

/* ---- configured PDU length (W40 bench: CAN FD pads 13..15 B to 16 B) ---- */

static size_t fake_pdu_len(uint32_t can_id)
{
    return (can_id == ID_DOOR) ? 1U : 0U;    /* ID_UNKNOWN deliberately unconfigured */
}

/* @test REQ-SECOC-001 : a genuine frame padded by CAN FD is accepted */
void test_padded_frame_is_accepted_with_the_pdu_len_lookup(void)
{
    secoc_rx_set_pdu_len_of(&g_rx, fake_pdu_len);
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32] = { 0U };
    size_t n = make_frame(ID_DOOR, g_fresh.floor, 3U, pdu, sizeof pdu, frame, sizeof frame);
    TEST_ASSERT_EQUAL_size_t(13U, n);

    uint8_t out[16]; size_t out_len = 0U;
    TEST_ASSERT_EQUAL(SECOC_RX_ACCEPT,
        secoc_rx_process(&g_rx, ID_DOOR, frame, 16U, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(1U, out_len);
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_ACCEPT, take_one().evt);
}

/* The regression the lookup fixes, pinned at the seam where the BVT sees it. */
/* @test REQ-LOG-009 */
void test_padded_frame_without_the_lookup_is_a_mac_reject(void)
{
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32] = { 0U };
    (void)make_frame(ID_DOOR, g_fresh.floor, 3U, pdu, sizeof pdu, frame, sizeof frame);

    uint8_t out[16]; size_t out_len = 0U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_MAC,
        secoc_rx_process(&g_rx, ID_DOOR, frame, 16U, out, sizeof out, &out_len));
    rec_t r = take_one();
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_MAC, r.evt);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)SECOC_BAD_MAC, r.arg1);
}

/* An ID the lookup does not know keeps the old path, so the unregistered-ID
 * verdict (a freshness drop, not a length error) is unchanged. */
/* @test REQ-SECOC-007 */
void test_unconfigured_id_keeps_the_freshness_drop_with_the_lookup(void)
{
    secoc_rx_set_pdu_len_of(&g_rx, fake_pdu_len);
    const uint8_t pdu[1] = { 0x01U };
    uint8_t frame[32];
    size_t n = make_frame(ID_UNKNOWN, g_fresh.floor, 1U, pdu, sizeof pdu, frame, sizeof frame);

    uint8_t out[16]; size_t out_len = 99U;
    TEST_ASSERT_EQUAL(SECOC_RX_DROP_FRESHNESS,
        secoc_rx_process(&g_rx, ID_UNKNOWN, frame, n, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(0U, out_len);
    TEST_ASSERT_EQUAL_HEX16(LOG_EVT_SECOC_REJECT_FRESHNESS, take_one().evt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_accept_emits_accept_event_with_id_and_counter);
    RUN_TEST(test_forged_mac_emits_reject_mac_reason_bad_mac);
    RUN_TEST(test_short_frame_emits_reject_mac_reason_bad_length);
    RUN_TEST(test_oracle_failure_is_distinguishable_from_a_forgery);
    RUN_TEST(test_unbound_receiver_drops_and_still_reports);
    RUN_TEST(test_replayed_frame_emits_reject_freshness_with_rx_counter);
    RUN_TEST(test_unregistered_can_id_is_a_freshness_drop);
    RUN_TEST(test_rejected_frame_yields_no_pdu);
    RUN_TEST(test_events_and_counters_never_diverge);
    RUN_TEST(test_sync_adopt_emits_resync_with_the_new_epoch);
    RUN_TEST(test_sync_adopt_is_silent_when_the_epoch_does_not_move);
    RUN_TEST(test_padded_frame_is_accepted_with_the_pdu_len_lookup);
    RUN_TEST(test_padded_frame_without_the_lookup_is_a_mac_reject);
    RUN_TEST(test_unconfigured_id_keeps_the_freshness_drop_with_the_lookup);
    return UNITY_END();
}
