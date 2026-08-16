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
#include "log.h"
#include "port_log.h"

#define LOG_UART_SCB            SCB0
#define LOG_UART_PCLK           PCLK_SCB0_CLOCK
#define LOG_UART_PORT           GPIO_PRT0
#define LOG_UART_RX_PIN         0U
#define LOG_UART_TX_PIN         1U

#define LOG_UART_BAUD           1000000UL
#define LOG_UART_OVERSAMPLE     8UL

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
volatile uint32_t g_log_actual_hz;
volatile uint32_t g_log_err_pm;
volatile bool     g_log_clk_ok;

static bool log_uart_clock_init(void)
{
    const uint32_t peri_hz = Cy_SysClk_ClkPeriGetFrequency();
    const uint32_t target  = LOG_UART_BAUD * LOG_UART_OVERSAMPLE;
    uint32_t       divider;
    uint32_t       actual;
    uint32_t       err_pm;

    g_log_peri_hz = peri_hz;
    g_log_clk_ok  = false;

    if ((peri_hz == 0U) || (target == 0U)) {
        return false;
    }

    divider = (peri_hz + (target / 2U)) / target;      /* round to nearest */
    if (divider == 0U) {
        divider = 1U;
    }

    actual = peri_hz / divider;
    err_pm = (actual > target) ? (((actual - target) * 1000U) / target)
                               : (((target - actual) * 1000U) / target);
    g_log_divider   = divider;
    g_log_actual_hz = actual / LOG_UART_OVERSAMPLE;   /* achieved baud */
    g_log_err_pm    = err_pm;

    if (err_pm > LOG_BAUD_TOLERANCE_PM) {
        return false;
    }
    g_log_clk_ok = true;

    (void)Cy_SysClk_PeriphAssignDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);
    (void)Cy_SysClk_PeriphSetDivider(LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM, divider - 1U);
    (void)Cy_SysClk_PeriphEnableDivider(LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);
    return true;
}

void fbl_log_init(void)
{
    static const cy_stc_scb_uart_config_t uart_cfg = {
        .uartMode                   = CY_SCB_UART_STANDARD,
        .enableMutliProcessorMode   = false,
        .smartCardRetryOnNack       = false,
        .irdaInvertRx               = false,
        .irdaEnableLowPowerReceiver = false,
        .oversample                 = LOG_UART_OVERSAMPLE,
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
