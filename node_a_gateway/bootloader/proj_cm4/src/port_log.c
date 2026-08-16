/*
 * port_log.c — FBL log port: SCB0 UART on the KitProg3 bridge (ADR-0023 D8).
 *
 * Same hardware and the same pins as the app's port (P0[0] RX / P0[1] TX ->
 * SCB0, HSIOM 17), because the FBL and the app are two CM4 images that never
 * run at once — the same reasoning that lets them share one IPC mailbox
 * (port_crypto.c). Two differences, both consequences of "no FreeRTOS here":
 *
 *   - the critical section masks with PRIMASK, not BASEPRI (see log_port_lock)
 *   - the clock is fbl_port_now_ms() (SysTick, port_time.c)
 *
 * WHY THE FBL LOGS AT ALL. LOG_EVT_APP_JUMP / LOG_EVT_APP_REJECT are the only
 * POSITIVE evidence the secure-boot BVT test has. "The FBL refused to jump" is
 * otherwise inferred from silence — and silence is also what a dead board, a
 * wedged FBL, or a board that never booted produces. Same argument as the SecOC
 * rejection events (ADR-0023 Context); this is the boot-time instance of it.
 *
 * There is no drain task here: the FBL is a super-loop. Bytes move when
 * fbl_log_service() is called from the programming-mode loop, and — critically
 * — via log_flush() immediately before the jump, because after the jump the app
 * re-inits SCB0 and anything still queued is gone.
 */
#include "fbl_port.h"        /* fbl_port_now_ms */
#include "cy_pdl.h"
#include "log_types.h"
#include "log_port.h"
#include "tb_log.h"
#include "port_log.h"

#define LOG_UART_SCB            SCB0
#define LOG_UART_PCLK           PCLK_SCB0_CLOCK
#define LOG_UART_PORT           GPIO_PRT0
#define LOG_UART_RX_PIN         0U
#define LOG_UART_TX_PIN         1U

#define LOG_UART_BAUD           1000000UL
/* SCB UART oversample range (PDL: 8..16 for standard UART). The exact value
 * is CHOSEN AT RUNTIME to hit the baud on whatever clock this core has. */
#define LOG_UART_OVS_MIN        8UL
#define LOG_UART_OVS_MAX        16UL

/* Must match the app's choice: the two images configure the same SCB from the
 * same divider, so a mismatch would only show up as a baud change across the
 * jump — readable in the FBL, garbage in the app, or vice versa. */
#define LOG_UART_DIV_TYPE       CY_SYSCLK_DIV_8_BIT
#define LOG_UART_DIV_NUM        0U
#define LOG_BAUD_TOLERANCE_PM   20U     /* 2% */

/* Smaller than the app's: the FBL logs a handful of records, not a stream. */
#define LOG_RING_CAP            512U

static uint8_t           s_ring[LOG_RING_CAP];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;

static bool s_sink_ready = false;

/* ---- ring / barrier ----------------------------------------------- */

uint8_t *log_port_ring(void)          { return s_ring; }
size_t   log_port_ring_cap(void)      { return (size_t)LOG_RING_CAP; }
volatile uint32_t *log_port_head(void){ return &s_head; }
volatile uint32_t *log_port_tail(void){ return &s_tail; }

void log_port_publish_barrier(void)              { __DMB(); }

/* ---- producer critical section ------------------------------------ */

/* PRIMASK rather than BASEPRI, deliberately.
 *
 * The app raises BASEPRI to configMAX_SYSCALL_INTERRUPT_PRIORITY so
 * higher-priority interrupts keep running. The FBL has no such priority
 * discipline to preserve — it is a super-loop with SysTick and the CAN ISR —
 * so the simplest correct thing is to mask everything for the ~16 bytes of a
 * record. A SysTick missed inside that window is not lost: it stays pending and
 * fires on unmask.
 *
 * Save and restore rather than a bare enable/disable pair, so nesting inside an
 * existing critical section does not wrongly re-enable interrupts on exit.
 */
uint32_t log_port_lock(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

void log_port_unlock(uint32_t state)
{
    __set_PRIMASK(state);
}

uint32_t log_port_now_ms(void)
{
    return fbl_port_now_ms();
}

/* ---- sink ---------------------------------------------------------- */

size_t log_port_tx(const uint8_t *data, size_t len)
{
    if (!s_sink_ready) {
        return 0U;
    }
    /* PutArray takes a non-const void*; it only reads. Contained deviation at
     * the vendor-API boundary, same as the app port. */
    return (size_t)Cy_SCB_UART_PutArray(LOG_UART_SCB, (void *)(uintptr_t)data, (uint32_t)len);
}

void log_port_tx_blocking(const uint8_t *data, size_t len)
{
    if (!s_sink_ready) {
        return;
    }
    Cy_SCB_UART_PutArrayBlocking(LOG_UART_SCB, (void *)(uintptr_t)data, (uint32_t)len);
    while (!Cy_SCB_UART_IsTxComplete(LOG_UART_SCB)) {
        /* spin: fault context only */
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

/* ---- bring-up ------------------------------------------------------ */

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

void fbl_log_init(void)
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
        return;     /* no usable divider: stay silent, never block the boot */
    }

    /* The SCB must be told the SAME oversample the divider was computed for --
     * they are two halves of one baud calculation, and disagreeing halves give
     * a silently wrong rate. */
    uart_cfg.oversample = s_oversample;

    Cy_GPIO_SetHSIOM(LOG_UART_PORT, LOG_UART_RX_PIN, P0_0_SCB0_UART_RX);
    Cy_GPIO_SetDrivemode(LOG_UART_PORT, LOG_UART_RX_PIN, CY_GPIO_DM_HIGHZ);
    Cy_GPIO_SetHSIOM(LOG_UART_PORT, LOG_UART_TX_PIN, P0_1_SCB0_UART_TX);
    Cy_GPIO_SetDrivemode(LOG_UART_PORT, LOG_UART_TX_PIN, CY_GPIO_DM_STRONG_IN_OFF);
    Cy_GPIO_Set(LOG_UART_PORT, LOG_UART_TX_PIN);

    if (Cy_SCB_UART_Init(LOG_UART_SCB, &uart_cfg, NULL) != CY_SCB_UART_SUCCESS) {
        return;
    }
    Cy_SCB_UART_Enable(LOG_UART_SCB);
    s_sink_ready = true;

    log_init(LOG_CORE_APP);
    log_banner();
}

void fbl_log_service(void)
{
    (void)log_drain(FBL_LOG_SERVICE_MAX_BYTES);
}
