/*
 * port_log.c — Node B CM7 log port: SCB0 UART on the KitProg3 bridge
 * (ADR-0023 D8, REQ-LOG-011).
 *
 * Mirrors node_a_gateway/app/src/port_log.c — same pins, same SCB, same
 * family-portable clock setup — because the port shape is deliberately one
 * shape for every image (cf. port_crypto.c, which mirrors the FBL's).
 *
 *      P0[0]  ->  SCB0 UART RX   (CYBSP_DEBUG_UART_RX in this BSP, INPUT)
 *      P0[1]  ->  SCB0 UART TX   (CYBSP_DEBUG_UART_TX, OUTPUT)
 *
 * Node B's BSP *does* configure SCB0 on these pins (unlike Node A's, which
 * configures no UART at all). We configure it here anyway: one port
 * implementation across all three images beats two, and it keeps this file
 * independent of a BSP that the CM0+ — not this core — applies.
 *
 * NO SHARED MEMORY, NO MPU STEP. The ring is image-private and is written and
 * drained by this core alone (ADR-0023 D4, revised): the security core does not
 * log, because per ADR-0021 D1 the SecOC verdict is decided here on the CM7, so
 * no contract event originates on the CM0+. That is why this file — unlike
 * port_crypto.c, which must place the IPC mailbox in an MPU non-cacheable
 * region (ADR-0018 D6) — needs no cache handling despite the CM7's L1 D-cache.
 *
 * ORDERING: fbl/app aside, this core reads 0 from the clock APIs until
 * SystemCoreClockUpdate() has run (see main.c — the PDL's frequency bookkeeping
 * is per-core and the CM7's globals start at 0). log_port_init() must be called
 * after that, or log_uart_clock_init() correctly refuses and the sink stays
 * silent.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "cy_pdl.h"
#include "log_types.h"
#include "log_port.h"
#include "tb_log.h"
#include "log_events.h"   /* LOG_EVT_BOOT — generated from events.csv */
#include "port_log.h"

/* -------------------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------------- */

#define LOG_UART_SCB            SCB0
#define LOG_UART_PCLK           PCLK_SCB0_CLOCK
#define LOG_UART_PORT           GPIO_PRT0
#define LOG_UART_RX_PIN         0U
#define LOG_UART_TX_PIN         1U

/* 1 Mbps, not the bridge's 4 Mbps ceiling (ADR-0023, "Baud rate"). At 16 B per
 * record that is ~6250 records/s — one to two orders of magnitude beyond any
 * path here — while 4 Mbps would need DMA to feed and would put the drain's
 * interrupt load on the core running the application. */
#define LOG_UART_BAUD           1000000UL
/* SCB UART oversample range (PDL: 8..16 for standard UART). The exact value
 * is CHOSEN AT RUNTIME to hit the baud on whatever clock this core has. */
#define LOG_UART_OVS_MIN        8UL
#define LOG_UART_OVS_MAX        16UL

/* Peripheral divider claimed for SCB0. A SHARED resource: if the BSP or another
 * peripheral on this node takes 8-bit divider 0, change this — do not share it.
 * Verify against the BSP's own divider assignments at bring-up. */
#define LOG_UART_DIV_TYPE       CY_SYSCLK_DIV_8_BIT
#define LOG_UART_DIV_NUM        0U

/* Worst tolerable baud error before framing gets unreliable, in per-mille. */
#define LOG_BAUD_TOLERANCE_PM   20U     /* 2% */

/* MUST be a power of two — log.c masks rather than divides. 2 KB = 128 records;
 * tune from log_dropped() (REQ-LOG-006), not by argument. Node B has 1 MB SRAM,
 * so there is no reason to be stingy here. */
#define LOG_RING_CAP            2048U

/* -------------------------------------------------------------------
 * Ring storage — ONE ring, image-private RAM.
 *
 * The security core does not log (ADR-0023 D4): per ADR-0021 D1 the SecOC
 * verdict is decided on THIS core, so no contract event originates on the CM0+.
 * A shared ring out of the core holding the AES secret would be TCB surface
 * with no evidence in return. Consequence: same-core producer and consumer, so
 * no shared-memory placement and no cache maintenance.
 * ----------------------------------------------------------------- */
static uint8_t           s_ring[LOG_RING_CAP];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;

static bool s_sink_ready = false;

/* -------------------------------------------------------------------
 * Ring / barrier
 * ----------------------------------------------------------------- */

uint8_t *log_port_ring(void)
{
    return s_ring;
}

size_t log_port_ring_cap(void)
{
    return (size_t)LOG_RING_CAP;
}

volatile uint32_t *log_port_head(void)
{
    return &s_head;
}

volatile uint32_t *log_port_tail(void)
{
    return &s_tail;
}

void log_port_publish_barrier(void)
{
    __DMB();
}

/* -------------------------------------------------------------------
 * Producer critical section
 *
 * Guards reserve+copy+publish as one indivisible step (ADR-0023 D7). It exists
 * because a task and an ISR on THIS core can both call log_evt(); the other
 * core has its own ring and needs no lock.
 *
 * portSET_INTERRUPT_MASK_FROM_ISR() raises BASEPRI to
 * configMAX_SYSCALL_INTERRUPT_PRIORITY and returns the PREVIOUS value, which
 * the matching clear restores. Two reasons to use it rather than a bare
 * __disable_irq(): BASEPRI masks only interrupts at or below the kernel's
 * syscall priority — higher-priority (non-kernel) interrupts and faults still
 * run — and save/restore composes correctly when nested inside an existing
 * critical section, where a blind re-enable would wrongly unmask.
 *
 * Despite the "_FROM_ISR" name these are valid in task context too, and they
 * touch no kernel object, so they are safe before the scheduler starts.
 * ----------------------------------------------------------------- */

uint32_t log_port_lock(void)
{
    return (uint32_t)portSET_INTERRUPT_MASK_FROM_ISR();
}

void log_port_unlock(uint32_t state)
{
    portCLEAR_INTERRUPT_MASK_FROM_ISR((UBaseType_t)state);
}

/* -------------------------------------------------------------------
 * Clock
 *
 * FreeRTOS tick, matching port_crypto.c. Before vTaskStartScheduler() this
 * reads 0 — early records therefore share a zero timestamp, which is correct
 * and honest: there is no time base yet. Ordering is still recoverable from
 * the per-core sequence number.
 *
 * Cross-core skew is uncorrected until the shared TCPWM time base lands
 * (ADR-0023, "Cross-core time base").
 * ----------------------------------------------------------------- */
uint32_t log_port_now_ms(void)
{
    return (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
}

/* -------------------------------------------------------------------
 * Sink
 * ----------------------------------------------------------------- */

/* Non-blocking. Cy_SCB_UART_PutArray() returns how many bytes it accepted into
 * the TX FIFO, which is exactly the partial-accept contract log_drain expects —
 * so do NOT loop here. Returning short is normal and means "busy". */
size_t log_port_tx(const uint8_t *data, size_t len)
{
    if (!s_sink_ready) {
        return 0U;
    }

    /* PutArray takes a non-const void*; it only reads. Casting away const is a
     * deliberate, contained deviation at the vendor-API boundary. */
    return (size_t)Cy_SCB_UART_PutArray(LOG_UART_SCB,
                                        (void *)(uintptr_t)data,
                                        (uint32_t)len);
}

/* Blocking, polled — log_panic() only (REQ-LOG-010). The one place logging may
 * spin, legitimate because it runs from a fault handler where no drain task
 * will ever execute. Waits for the shifter to empty so the record actually
 * leaves the pin before the caller resets or halts. */
void log_port_tx_blocking(const uint8_t *data, size_t len)
{
    if (!s_sink_ready) {
        return;
    }

    Cy_SCB_UART_PutArrayBlocking(LOG_UART_SCB, (void *)(uintptr_t)data, (uint32_t)len);

    while (!Cy_SCB_UART_IsTxComplete(LOG_UART_SCB)) {
        /* spin: fault context, nothing else is going to run */
    }
}

bool log_port_sink_ready(void)
{
    return s_sink_ready;
}

/* Transmit COMPLETE, not "FIFO accepted it". log_flush() waits on this before a
 * jump or reset, because whoever re-inits SCB0 next resets the FIFO and would
 * otherwise destroy records that are still shifting out. */
bool log_port_sink_idle(void)
{
    if (!s_sink_ready) {
        return true;      /* nothing can be in flight if the sink never came up */
    }
    return Cy_SCB_UART_IsTxComplete(LOG_UART_SCB);
}

/* -------------------------------------------------------------------
 * Bring-up
 * ----------------------------------------------------------------- */

/* Choose the peripheral-clock divider for the target baud and check the result
 * is within tolerance. Computed at runtime from the actual CLK_PERI rather than
 * hardcoded: the clock tree is configured elsewhere and may change, and a
 * silently wrong divider shows up as unreadable output, not as a build error.
 *
 * Returns false if no acceptable divider exists — the caller then leaves the
 * sink down rather than emitting garbage. */
/* Bring-up aids: the numbers behind the baud, readable in the debugger. Same
 * pattern as g_hw_health / g_assert_file / g_tcb_probe.
 *
 * These exist because a wrong baud is INVISIBLE from the console -- the console
 * is the thing that breaks. If the FBL and the app disagree on CLK_PERI, each
 * computes a different divider, and only one of them is readable at the
 * terminal's fixed rate. Read g_log_peri_hz in both images and compare. */
volatile uint32_t g_log_peri_hz;
volatile uint32_t g_log_divider;
volatile uint32_t g_log_oversample;
volatile uint32_t g_log_actual_hz;
volatile uint32_t g_log_err_pm;
volatile bool     g_log_clk_ok;

/* Chosen at runtime by log_uart_clock_init(), consumed by the SCB config. */
static uint32_t s_oversample = LOG_UART_OVS_MIN;

static bool log_uart_clock_init(void)
{
    uint32_t src_hz;
    uint32_t best_ovs = 0U;
    uint32_t best_div = 0U;
    uint32_t best_err = 0xFFFFFFFFUL;
    uint32_t ovs;

    g_log_clk_ok = false;

    /* Measure, do not assume. Set divide-by-1 first and READ BACK what the
     * destination actually receives: that yields the source frequency without
     * this file having to know the clock tree, which differs between the two
     * nodes (CAT1A/TVIIBE vs CAT1C) and, on Node B, is configured by the CM0+.
     *
     * Cy_SysClk_PeriPclk* is the family-portable API -- the PDL documents it for
     * "CAT1A (TVIIBE only), CAT1B, CAT1C and CAT1D" -- so all three images share
     * one implementation instead of each guessing its own accessor. */
    (void)Cy_SysClk_PeriPclkAssignDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);
    (void)Cy_SysClk_PeriPclkSetDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM, 0U);
    (void)Cy_SysClk_PeriPclkEnableDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);

    src_hz        = Cy_SysClk_PeriPclkGetFrequency(LOG_UART_PCLK, LOG_UART_DIV_TYPE,
                                                   LOG_UART_DIV_NUM);
    g_log_peri_hz = src_hz;

    /* Zero means the clock tree is not up yet on this core -- on Node B the CM7
     * reads 0 until SystemCoreClockUpdate() runs. Leave the sink down rather
     * than emit at a garbage rate. */
    if (src_hz == 0U) {
        return false;
    }

    /* SEARCH the oversample, do not fix it.
     *
     * A single hardcoded oversample ties the achievable baud to one clock tree.
     * Silicon proved it: at Node A's 80 MHz, oversample 8 divides by exactly 10
     * and is perfect; at Node B's 100 MHz the same 8 needs 12.5, rounds to 13,
     * and lands 3.8% off -- unusable. But 100 MHz / 1 Mbps = 100 = 10 x 10, so
     * an oversample of 10 is EXACT. Searching 8..16 finds the best pair on any
     * clock tree, which is what lets one port serve every image.
     *
     * Node A is unaffected: the search hits err = 0 at oversample 8 and stops
     * there, reproducing the configuration already verified on that board. */
    for (ovs = LOG_UART_OVS_MIN; ovs <= LOG_UART_OVS_MAX; ovs++) {
        const uint32_t target = LOG_UART_BAUD * ovs;
        uint32_t       div;
        uint32_t       actual;
        uint32_t       err_pm;

        div = (src_hz + (target / 2U)) / target;   /* round to nearest */
        if (div == 0U) {
            div = 1U;
        }

        actual = src_hz / div;                     /* achieved SCB clock */
        err_pm = (actual > target) ? (((actual - target) * 1000U) / target)
                                   : (((target - actual) * 1000U) / target);

        if (err_pm < best_err) {
            best_err = err_pm;
            best_ovs = ovs;
            best_div = div;
        }
        if (err_pm == 0U) {
            break;                                 /* exact -- stop looking */
        }
    }

    (void)Cy_SysClk_PeriPclkSetDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM,
                                       best_div - 1U);

    /* Read back the ACHIEVED clock rather than trusting the arithmetic. */
    g_log_divider   = best_div;
    g_log_oversample = best_ovs;
    g_log_actual_hz = Cy_SysClk_PeriPclkGetFrequency(LOG_UART_PCLK, LOG_UART_DIV_TYPE,
                                                     LOG_UART_DIV_NUM) / best_ovs;
    g_log_err_pm    = best_err;

    if (best_err > LOG_BAUD_TOLERANCE_PM) {
        return false;
    }

    s_oversample = best_ovs;
    g_log_clk_ok = true;
    return true;
}

static void log_uart_pins_init(void)
{
    /* RX: input, no drive. TX: strong drive, input buffer off. Matches the
     * drive modes Node B's BSP applies to the same two pins. */
    Cy_GPIO_SetHSIOM(LOG_UART_PORT, LOG_UART_RX_PIN, P0_0_SCB0_UART_RX);
    Cy_GPIO_SetDrivemode(LOG_UART_PORT, LOG_UART_RX_PIN, CY_GPIO_DM_HIGHZ);

    Cy_GPIO_SetHSIOM(LOG_UART_PORT, LOG_UART_TX_PIN, P0_1_SCB0_UART_TX);
    Cy_GPIO_SetDrivemode(LOG_UART_PORT, LOG_UART_TX_PIN, CY_GPIO_DM_STRONG_IN_OFF);
    Cy_GPIO_Set(LOG_UART_PORT, LOG_UART_TX_PIN);   /* idle high, avoids a start-bit glitch */
}

void log_port_init(void)
{
    cy_stc_scb_uart_config_t uart_cfg = {
        .uartMode                   = CY_SCB_UART_STANDARD,
        .enableMutliProcessorMode   = false,
        .smartCardRetryOnNack       = false,
        .irdaInvertRx               = false,
        .irdaEnableLowPowerReceiver = false,
        .oversample                 = LOG_UART_OVS_MIN,   /* overwritten below */
        .enableMsbFirst             = false,
        .dataWidth                  = 8UL,
        .parity                     = CY_SCB_UART_PARITY_NONE,
        .stopBits                   = CY_SCB_UART_STOP_BITS_1,
        .enableInputFilter          = false,
        .breakWidth                 = 11UL,
        .dropOnFrameError           = false,
        .dropOnParityError          = false,
        .receiverAddress            = 0UL,
        .receiverAddressMask        = 0UL,
        .acceptAddrInFifo           = false,
        .enableCts                  = false,
        .ctsPolarity                = CY_SCB_UART_ACTIVE_LOW,
        .rtsRxFifoLevel             = 0UL,
        .rtsPolarity                = CY_SCB_UART_ACTIVE_LOW,
        .rxFifoTriggerLevel         = 0UL,
        .rxFifoIntEnableMask        = 0UL,
        .txFifoTriggerLevel         = 0UL,
        .txFifoIntEnableMask        = 0UL
    };

    s_sink_ready = false;

    if (!log_uart_clock_init()) {
        /* No usable divider. Leave the sink down: log_evt() keeps queueing and
         * dropping harmlessly (REQ-LOG-005), and nothing else in the app is
         * affected. Silence is the correct failure mode for a log channel —
         * it must never take the application down with it. */
        return;
    }

    /* The SCB must be told the SAME oversample the divider was computed for --
     * they are two halves of one baud calculation, and disagreeing halves give
     * a silently wrong rate. */
    uart_cfg.oversample = s_oversample;

    log_uart_pins_init();

    if (Cy_SCB_UART_Init(LOG_UART_SCB, &uart_cfg, NULL) != CY_SCB_UART_SUCCESS) {
        return;
    }
    Cy_SCB_UART_Enable(LOG_UART_SCB);

    s_sink_ready = true;

    /* Ring + banner. Producers may run before this; their records simply queue,
     * and overflow drops the NEWEST so the earliest survive (REQ-LOG-006). */
    log_init(LOG_CORE_APP);

    /* Announce the image start. Not decoration: log_init() zeroed the sequence
     * counter, and LOG_EVT_BOOT is how the host decoder knows a discontinuity
     * is a RESTART rather than lost records — without it every power cycle
     * reports a false "records LOST", which under REQ-LOG-007 fails a
     * perfectly good BVT window. */
    log_evt(LOG_EVT_BOOT, (uint32_t)Cy_SysLib_GetResetReason(), 0U);
}
