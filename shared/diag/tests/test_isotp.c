/*
 * test_isotp.c — Unity tests for the ISO-TP transport (ADR-0012 D1/D2,
 * ADR-0013).
 *
 * Covers single-frame and multi-frame reassembly, out-of-order/timeout
 * rejection (the transport-level half of the interrupted-transfer safety
 * story -- ADR-0012 D5), oversized-message rejection, and basic TX
 * segmentation against a flow-control CTS. Fails against the current stub
 * isotp.c (step 5 implements). Test harness — exempt from MISRA.
 */
#include "unity.h"
#include "isotp.h"
#include "isotp_types.h"
#include "can_hal.h"
#include <string.h>

extern void fake_can_reset(void);
extern void fake_can_inject_rx(const can_raw_frame_t *f);
extern size_t fake_can_sent_count(void);
extern const can_raw_frame_t *fake_can_sent_frame(size_t idx);
extern const can_hal_if_t *fake_can_hal(void);

#define TEST_TX_ID  0x7A8U   /* Node A diagnostic response ID, ADR-0002 M3 extension */

void setUp(void)
{
    fake_can_reset();
    isotp_init(fake_can_hal(), TEST_TX_ID);
}
void tearDown(void) {}

/* ---- frame builders (PCI layout, isotp_types.h) ---- */
static can_raw_frame_t make_sf(const uint8_t *payload, uint8_t len)
{
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.flags = CAN_FLAG_FDF;
    f.data[0] = ISOTP_PCI_TYPE_SF;
    f.data[1] = len;
    memcpy(&f.data[2], payload, len);
    f.len = (uint8_t)(2U + len);
    return f;
}

static can_raw_frame_t make_ff(uint32_t total_len, const uint8_t *initial, uint8_t initial_len)
{
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.flags = CAN_FLAG_FDF;
    f.data[0] = ISOTP_PCI_TYPE_FF;
    f.data[1] = (uint8_t)(total_len >> 24);
    f.data[2] = (uint8_t)(total_len >> 16);
    f.data[3] = (uint8_t)(total_len >> 8);
    f.data[4] = (uint8_t)(total_len);
    memcpy(&f.data[5], initial, initial_len);
    f.len = (uint8_t)(5U + initial_len);
    return f;
}

static can_raw_frame_t make_cf(uint8_t seq, const uint8_t *payload, uint8_t len)
{
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.flags = CAN_FLAG_FDF;
    f.data[0] = (uint8_t)(ISOTP_PCI_TYPE_CF | (seq & 0x0FU));
    memcpy(&f.data[1], payload, len);
    f.len = (uint8_t)(1U + len);
    return f;
}

static can_raw_frame_t make_fc(uint8_t status, uint8_t bs, uint8_t stmin)
{
    can_raw_frame_t f;
    memset(&f, 0, sizeof f);
    f.flags = CAN_FLAG_FDF;
    f.data[0] = (uint8_t)(ISOTP_PCI_TYPE_FC | (status & 0x0FU));
    f.data[1] = bs;
    f.data[2] = stmin;
    f.len = 3U;
    return f;
}

/* ==================================================================
 * RX reassembly
 * ================================================================ */
void test_rx_single_frame_completes_immediately(void)
{
    uint8_t payload[10] = { 1,2,3,4,5,6,7,8,9,10 };
    can_raw_frame_t f = make_sf(payload, sizeof payload);
    fake_can_inject_rx(&f);

    isotp_poll(0U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_TRUE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(sizeof payload, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, sizeof payload);
}

void test_rx_multiframe_completes_and_sends_fc_cts(void)
{
    uint8_t msg[100];
    for (size_t i = 0; i < sizeof msg; ++i) { msg[i] = (uint8_t)i; }

    can_raw_frame_t ff = make_ff(sizeof msg, msg, 59U);
    fake_can_inject_rx(&ff);
    isotp_poll(0U);

    /* A reception needing more data must request it (D2: FC sent
     * synchronously from within poll). */
    TEST_ASSERT_EQUAL_size_t(1U, fake_can_sent_count());
    const can_raw_frame_t *fc = fake_can_sent_frame(0U);
    TEST_ASSERT_EQUAL_HEX32(TEST_TX_ID, fc->id);
    TEST_ASSERT_EQUAL_HEX8(ISOTP_PCI_TYPE_FC | ISOTP_FC_STATUS_CTS, fc->data[0]);

    can_raw_frame_t cf = make_cf(1U, &msg[59], 41U);
    fake_can_inject_rx(&cf);
    isotp_poll(10U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_TRUE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(sizeof msg, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, out, sizeof msg);
}

void test_rx_cf_ignores_dlc_padding_beyond_logical_length(void)
{
    /* A CAN FD frame's DLC only represents specific lengths (0-8, then
     * 12/16/20/24/32/48/64) -- a CF whose real payload doesn't land exactly
     * on one of those gets padded on the wire (e.g. 41 real bytes + 1 PCI =
     * 42, rounds up to 48). A receiver must derive how many bytes are real
     * from the ISO-TP logical state (bytes still needed), not from the
     * frame's own reported length, or it treats trailing pad bytes as
     * message content (silicon-verified during Seam 2 bring-up). */
    uint8_t msg[100];
    for (size_t i = 0; i < sizeof msg; ++i) { msg[i] = (uint8_t)i; }

    can_raw_frame_t ff = make_ff(sizeof msg, msg, 59U);
    fake_can_inject_rx(&ff);
    isotp_poll(0U);

    can_raw_frame_t cf;
    memset(&cf, 0, sizeof cf);
    cf.flags = CAN_FLAG_FDF;
    cf.data[0] = (uint8_t)(ISOTP_PCI_TYPE_CF | 1U);
    memcpy(&cf.data[1], &msg[59], 41U);
    memset(&cf.data[42], 0xEEU, 6U);   /* padding garbage -- must not be copied */
    cf.len = 48U;                       /* wire length rounds 42 real bytes up to 48 */
    fake_can_inject_rx(&cf);
    isotp_poll(10U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_TRUE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_size_t(sizeof msg, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, out, sizeof msg);
}

void test_rx_cf_wrong_sequence_aborts_reception(void)
{
    uint8_t msg[100] = { 0 };
    can_raw_frame_t ff = make_ff(sizeof msg, msg, 59U);
    fake_can_inject_rx(&ff);
    isotp_poll(0U);

    /* Sequence should be 1 next; send 2 instead -- must not be silently
     * accepted (the transport-level half of the interrupted-transfer safety
     * property, ADR-0012 D5). */
    can_raw_frame_t bad_cf = make_cf(2U, &msg[59], 41U);
    fake_can_inject_rx(&bad_cf);
    isotp_poll(10U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_FALSE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL(ISOTP_ERROR_FLOW, isotp_current_state());
}

void test_rx_ncr_timeout_aborts_reception(void)
{
    uint8_t msg[100] = { 0 };
    can_raw_frame_t ff = make_ff(sizeof msg, msg, 59U);
    fake_can_inject_rx(&ff);
    isotp_poll(0U);   /* sends FC, starts waiting for CF */

    /* No CF ever arrives; time passes N_Cr. */
    isotp_poll(ISOTP_N_CR_MS + 1U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_FALSE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL(ISOTP_ERROR_TIMEOUT, isotp_current_state());
}

void test_rx_oversized_ff_rejected(void)
{
    uint8_t initial[59] = { 0 };
    can_raw_frame_t ff = make_ff((uint32_t)ISOTP_MAX_PAYLOAD + 1U, initial, sizeof initial);
    fake_can_inject_rx(&ff);
    isotp_poll(0U);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0U;
    TEST_ASSERT_FALSE(isotp_take_received(out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL(ISOTP_ERROR_OVERFLOW, isotp_current_state());
}

/* ==================================================================
 * TX segmentation
 * ================================================================ */
void test_tx_small_payload_is_single_frame(void)
{
    uint8_t payload[10] = { 9,8,7,6,5,4,3,2,1,0 };
    TEST_ASSERT_EQUAL_INT(0, isotp_send(payload, sizeof payload));

    TEST_ASSERT_EQUAL_size_t(1U, fake_can_sent_count());
    const can_raw_frame_t *f = fake_can_sent_frame(0U);
    TEST_ASSERT_EQUAL_HEX32(TEST_TX_ID, f->id);
    TEST_ASSERT_EQUAL_HEX8(ISOTP_PCI_TYPE_SF, f->data[0]);
    TEST_ASSERT_EQUAL_UINT8(sizeof payload, f->data[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &f->data[2], sizeof payload);
}

void test_tx_large_payload_sends_ff_first(void)
{
    uint8_t msg[100];
    for (size_t i = 0; i < sizeof msg; ++i) { msg[i] = (uint8_t)(i + 1U); }

    TEST_ASSERT_EQUAL_INT(0, isotp_send(msg, sizeof msg));

    TEST_ASSERT_EQUAL_size_t(1U, fake_can_sent_count());
    const can_raw_frame_t *ff = fake_can_sent_frame(0U);
    TEST_ASSERT_EQUAL_HEX32(TEST_TX_ID, ff->id);
    TEST_ASSERT_EQUAL_HEX8(ISOTP_PCI_TYPE_FF, ff->data[0]);
    uint32_t total_len = ((uint32_t)ff->data[1] << 24) | ((uint32_t)ff->data[2] << 16) |
                         ((uint32_t)ff->data[3] << 8)  |  (uint32_t)ff->data[4];
    TEST_ASSERT_EQUAL_UINT32(sizeof msg, total_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, &ff->data[5], 59U);
}

void test_tx_completes_after_fc_cts(void)
{
    uint8_t msg[100];
    for (size_t i = 0; i < sizeof msg; ++i) { msg[i] = (uint8_t)(i + 1U); }
    isotp_send(msg, sizeof msg);

    can_raw_frame_t fc = make_fc(ISOTP_FC_STATUS_CTS, ISOTP_BLOCK_SIZE, ISOTP_STMIN_MS);
    fake_can_inject_rx(&fc);
    isotp_poll(0U);

    TEST_ASSERT_EQUAL_size_t(2U, fake_can_sent_count());
    const can_raw_frame_t *cf = fake_can_sent_frame(1U);
    TEST_ASSERT_EQUAL_HEX32(TEST_TX_ID, cf->id);
    TEST_ASSERT_EQUAL_HEX8(ISOTP_PCI_TYPE_CF | 1U, cf->data[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&msg[59], &cf->data[1], 41U);
}

/* ==================================================================
 * Runner
 * ================================================================ */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_rx_single_frame_completes_immediately);
    RUN_TEST(test_rx_multiframe_completes_and_sends_fc_cts);
    RUN_TEST(test_rx_cf_ignores_dlc_padding_beyond_logical_length);
    RUN_TEST(test_rx_cf_wrong_sequence_aborts_reception);
    RUN_TEST(test_rx_ncr_timeout_aborts_reception);
    RUN_TEST(test_rx_oversized_ff_rejected);

    RUN_TEST(test_tx_small_payload_is_single_frame);
    RUN_TEST(test_tx_large_payload_sends_ff_first);
    RUN_TEST(test_tx_completes_after_fc_cts);

    return UNITY_END();
}
