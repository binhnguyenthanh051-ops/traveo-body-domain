/*
 * port_can.c — the FBL's CANFD bring-up + knock-frame poll (target). M3
 * Seam 1 (ADR-0004, ADR-0011, ADR-0013 D5).
 *
 * Polled, not ISR+queue (ADR-0004: the FBL is a super-loop, no RTOS to hand
 * work to) -- this is the deliberate fork from the App's can_task.c pattern
 * called out in ADR-0012 D2/D6. Same physical channel/pins/bitrates as the
 * App (ADR-0011 D1: "the FBL knock path... uses identical parameters; M2
 * sets these shared parameters, it does not pick numbers the FBL would have
 * to contradict") -- CANFD0 channel 1, P0.2 (TX) / P0.3 (RX), 500 kbit/s
 * nominal / 2 Mbit/s data.
 *
 * Device Configurator personality (canfd_0_chan_1) generated and building
 * clean as of this Seam's bring-up -- config verified against the App's own
 * known-good M2 values (timing, pins, RAM sizing all match).
 *
 * Bring-up counters below (g_can_rx_count etc.) mirror can_task.c's own
 * "stage counters" pattern: read them in the debugger to confirm the RX path
 * is alive independent of the knock window's 2 s timing, before trusting the
 * timing-sensitive behavioural test (does a knock actually keep the FBL
 * resident).
 */
#include "fbl_can.h"
#include "fbl_port.h"
#include "fbl_config.h"    /* FBL_KNOCK_CAN_ID */
#include "cybsp.h"
#include "cy_pdl.h"        /* Cy_CANFD_*, CANFD_RXF0S */
#include <string.h>

#define CAN_HW_INSTANCE      CANFD0
#define CAN_HW_CHANNEL       1U
#define CAN_TX_BUF_IDX       0U

/* Bring-up only (Seam 1) -- read in the debugger. Not gated behind a build
 * flag like can_task.c's CAN_LOOPBACK_TEST; harmless to leave in, cheap to
 * strip out once the seam is trusted. */
volatile uint32_t g_can_rx_count;    /* frames successfully popped by hal_recv() */
volatile uint32_t g_can_last_id;     /* id of the last one */
volatile int32_t  g_can_init_status; /* Cy_CANFD_Init return (0 = CY_CANFD_SUCCESS) */

static cy_stc_canfd_context_t s_canfd_context;

/* CAN FD DLC <-> byte-length (mirrors can_task.c's table -- both images pack
 * frames the same way, worth keeping identical rather than re-deriving). */
static uint8_t dlc_to_len(uint32_t dlc)
{
    static const uint8_t lut[16] = { 0U,1U,2U,3U,4U,5U,6U,7U,8U,
                                     12U,16U,20U,24U,32U,48U,64U };
    return lut[dlc & 0x0FU];
}

static uint32_t len_to_dlc(uint8_t len)
{
    uint32_t dlc;
    if      (len <= 8U)  { dlc = len; }
    else if (len <= 12U) { dlc = 9U;  }
    else if (len <= 16U) { dlc = 10U; }
    else if (len <= 20U) { dlc = 11U; }
    else if (len <= 24U) { dlc = 12U; }
    else if (len <= 32U) { dlc = 13U; }
    else if (len <= 48U) { dlc = 14U; }
    else                 { dlc = 15U; }
    return dlc;
}

static void do_init(void)
{
    /* Cy_CANFD_Init also enables the channel (per the PDL note, ADR-0011);
     * config comes from the Device Configurator, not a runtime can_cfg_t --
     * the bit-timing/message-RAM values are fixed at generation time, same
     * as the App (ADR-0011 D1/D2). */
    g_can_init_status = (int32_t)Cy_CANFD_Init(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                               &canfd_0_chan_1_config, &s_canfd_context);
}

static int hal_init(const can_cfg_t *cfg)
{
    (void)cfg;
    do_init();
    return 0;
}

/* With a single dedicated TX buffer (ADR-0011 D4), a caller that sends
 * several frames back-to-back (e.g. isotp.c's tx_send_block(), a CF burst)
 * can ask for the next send before the previous one has actually gone out --
 * TXBRP (TX Buffer Request Pending) bit CAN_TX_BUF_IDX stays set until that
 * happens. Reusing the buffer before it clears drops or corrupts the
 * in-flight frame (silicon-verified during Seam 2 bring-up: the second of
 * two back-to-back consecutive frames never reached the bus). Bounded to
 * 10 ms -- a single frame at 500 kbit/s nominal / 2 Mbit/s data completes in
 * tens of microseconds, so this is a generous margin, not a real budget. */
#define TX_BUF_WAIT_TIMEOUT_MS  10U

/* Bounded wait for the dedicated TX buffer to be free (TXBRP clear) -- either
 * before reusing it for a new send, or before something external (a system
 * reset) needs the in-flight frame to have actually finished going out on
 * the wire first. Sharing one wait here means both call sites agree on the
 * same timeout/margin reasoning. Returns true if the buffer became free,
 * false if the bounded wait was exhausted first. */
static bool wait_tx_buf_free(void)
{
    uint32_t start = fbl_port_now_ms();
    while ((CANFD_TXBRP(CAN_HW_INSTANCE, CAN_HW_CHANNEL) & (1UL << CAN_TX_BUF_IDX)) != 0U)
    {
        if ((fbl_port_now_ms() - start) >= TX_BUF_WAIT_TIMEOUT_MS) { return false; }
    }
    return true;
}

/* Best-effort: used before a system reset (ADR-0007 D10's ECUReset sequence)
 * needs the just-sent response to have actually left the buffer first. If
 * the wait times out there is nothing more to be gained by not resetting --
 * the reset proceeds either way. */
void fbl_can_wait_tx_complete(void)
{
    (void)wait_tx_buf_free();
}

static int hal_send(const can_raw_frame_t *f)
{
    if (!wait_tx_buf_free()) { return -1; }

    cy_stc_canfd_t0_t t0 = { 0 };
    cy_stc_canfd_t1_t t1 = { 0 };
    uint32_t          data[CAN_MAX_DLEN / 4U] = { 0 };
    cy_stc_canfd_tx_buffer_t tx = { .t0_f = &t0, .t1_f = &t1, .data_area_f = data };

    t0.id  = f->id;
    t0.rtr = ((f->flags & CAN_FLAG_RTR) != 0U) ? CY_CANFD_RTR_REMOTE_FRAME : CY_CANFD_RTR_DATA_FRAME;
    t0.xtd = ((f->flags & CAN_FLAG_IDE) != 0U) ? CY_CANFD_XTD_EXTENDED_ID  : CY_CANFD_XTD_STANDARD_ID;
    t1.dlc = len_to_dlc(f->len);
    t1.fdf = ((f->flags & CAN_FLAG_FDF) != 0U) ? CY_CANFD_FDF_CAN_FD_FRAME : CY_CANFD_FDF_STANDARD_FRAME;
    t1.brs = ((f->flags & CAN_FLAG_BRS) != 0U);
    (void)memcpy(data, f->data, f->len);

    return (Cy_CANFD_UpdateAndTransmitMsgBuffer(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                                &tx, CAN_TX_BUF_IDX, &s_canfd_context)
            == CY_CANFD_SUCCESS) ? 0 : -1;
}

/* Polled RX FIFO 0 read (no ISR): check the fill level (RXF0S.F0FL) before
 * reading. Extraction goes through Cy_CANFD_ExtractMsgFromRXBuffer, not a
 * hand-rolled Cy_CANFD_GetFIFOTop()+Cy_CANFD_AckRxFifo() pair -- the
 * Device Configurator has topPointerLogicEnabledFifo0 = false (both this
 * project and the App's), so the "top pointer" register GetFIFOTop reads
 * from is not the active mechanism here; ExtractMsgFromRXBuffer branches on
 * that setting internally (computing the real element address via
 * Cy_CANFD_CalcRxFifoAdrs + Cy_CANFD_GetRxBuffer, and acknowledging via
 * RXF0A) and gets it right either way, which a direct GetFIFOTop call does
 * not (silicon-verified during Seam 1 bring-up: it returned CY_CANFD_SUCCESS
 * with an all-zero payload). */
static int hal_recv(can_raw_frame_t *out)
{
    uint32_t rxf0s = CANFD_RXF0S(CAN_HW_INSTANCE, CAN_HW_CHANNEL);
    if (_FLD2VAL(CANFD_CH_M_TTCAN_RXF0S_F0FL, rxf0s) == 0U)
    {
        return -1;   /* nothing pending */
    }

    cy_stc_canfd_r0_t r0 = { 0 };
    cy_stc_canfd_r1_t r1 = { 0 };
    uint32_t          data[CAN_MAX_DLEN / 4U] = { 0 };
    cy_stc_canfd_rx_buffer_t rx = { .r0_f = &r0, .r1_f = &r1, .data_area_f = data };

    if (Cy_CANFD_ExtractMsgFromRXBuffer(CAN_HW_INSTANCE, CAN_HW_CHANNEL,
                                        true, (uint8_t)CY_CANFD_RX_FIFO0,
                                        &rx, &s_canfd_context)
        != CY_CANFD_SUCCESS)
    {
        return -1;
    }

    out->id = r0.id;
    out->flags = 0U;
    if (r0.xtd == CY_CANFD_XTD_EXTENDED_ID)  { out->flags |= CAN_FLAG_IDE; }
    if (r0.rtr == CY_CANFD_RTR_REMOTE_FRAME) { out->flags |= CAN_FLAG_RTR; }
    if (r1.fdf == CY_CANFD_FDF_CAN_FD_FRAME) { out->flags |= CAN_FLAG_FDF; }
    if (r1.brs)                               { out->flags |= CAN_FLAG_BRS; }
    out->len = dlc_to_len(r1.dlc);
    (void)memcpy(out->data, data, out->len);

    ++g_can_rx_count;
    g_can_last_id = out->id;
    return 0;
}

static const can_hal_if_t g_fbl_can_hal = {
    .init = hal_init,
    .send = hal_send,
    .recv = hal_recv
};

void fbl_can_init(void) { do_init(); }
const can_hal_if_t *fbl_can_hal(void) { return &g_fbl_can_hal; }

/* fbl_port_tool_contact (ADR-0008 D2, ADR-0013 D5): presence detection only
 * -- checks for any inbound frame on the diagnostic ID, does not parse it as
 * a UDS request. The real session/handler stack (Seam 4+) runs only after
 * the FBL has committed to staying resident; a tester whose knock frame IS
 * its first real DiagnosticSessionControl should be prepared to resend it
 * once programming mode starts -- this function does not buffer or forward
 * what it consumes. Drains all pending frames per call so a burst during the
 * window can't stall behind a non-matching one. */
bool fbl_port_tool_contact(void)
{
    can_raw_frame_t f;
    bool contact = false;
    while (hal_recv(&f) == 0)
    {
        if (f.id == FBL_KNOCK_CAN_ID) { contact = true; }
    }
    return contact;
}
