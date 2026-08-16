/*
 * port_log.c — Node A APP log port: SCB0 UART on the KitProg3 bridge
 * (ADR-0023 D8, REQ-LOG-011).
 *
 * Pin/instance mapping (confirmed against the PDL HSIOM table
 * gpio_tviibe1m_*.h, and identical on Node B):
 *
 *      P0[0]  ->  SCB0 UART RX   (HSIOM P0_0_SCB0_UART_RX, target INPUT)
 *      P0[1]  ->  SCB0 UART TX   (HSIOM P0_1_SCB0_UART_TX, target OUTPUT)
 *
 * WHY THIS IS CONFIGURED IN CODE AND NOT IN THE BSP
 * -------------------------------------------------
 * Node A's BSP configures no UART at all, and per .gitignore the whole
 * node_a_gateway/app/bsps/ tree is REGENERABLE and deliberately not committed
 * ("its only customisation ... is committed as app/linker/app_cm4.ld"). A UART
 * added there via Device Configurator is silently lost on the next BSP
 * regeneration — and the failure mode is a console that stops existing, which
 * is exactly when you need it. So the port owns its own hardware setup. Node B
 * (whose BSP *does* configure SCB0) is set up the same way on purpose, so one
 * port shape serves every image.
 *
 * The M4 has no cache, so the rings are ordinary RAM — no MPU non-cacheable
 * step (cf. port_crypto.c; that step is Node B's CM7 problem, ADR-0018 D6).
 */
#include "FreeRTOS.h"
#include "task.h"
#include "cy_pdl.h"
#include "log_types.h"
#include "log_port.h"
#include "log.h"
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
#define LOG_UART_OVERSAMPLE     8UL

/* Peripheral divider claimed for SCB0. Neither Node A image allocates an 8-bit
 * divider today, so index 0 is free — but it is a SHARED resource: if another
 * peripheral later takes 8-bit divider 0, change this, don't share it. */
#define LOG_UART_DIV_TYPE       CY_SYSCLK_DIV_8_BIT
#define LOG_UART_DIV_NUM        0U

/* Worst tolerable baud error before framing gets unreliable, in per-mille. */
#define LOG_BAUD_TOLERANCE_PM   20U     /* 2% */

/* Ring capacity per core. MUST be a power of two — log.c masks rather than
 * divides. 2 KB = 128 records; tune from log_dropped() (REQ-LOG-006), not by
 * argument. */
#define LOG_RING_CAP            2048U

/* -------------------------------------------------------------------
 * Ring storage
 *
 * Two rings, one per producing core (log_drain walks both). The SECURITY ring
 * has no producer in this image yet — the CM0+ side lands in a later step, at
 * which point this array moves to the shared region the boot handshake already
 * uses. Until then it stays empty, and drain skips it in O(1).
 * ----------------------------------------------------------------- */
static uint8_t           s_ring[LOG_CORE_COUNT][LOG_RING_CAP];
static volatile uint32_t s_head[LOG_CORE_COUNT];
static volatile uint32_t s_tail[LOG_CORE_COUNT];

static bool s_sink_ready = false;

/* -------------------------------------------------------------------
 * Ring / barrier
 * ----------------------------------------------------------------- */

uint8_t *log_port_ring(log_core_t core)
{
    return s_ring[core];
}

size_t log_port_ring_cap(log_core_t core)
{
    (void)core;
    return (size_t)LOG_RING_CAP;
}

volatile uint32_t *log_port_head(log_core_t core)
{
    return &s_head[core];
}

volatile uint32_t *log_port_tail(log_core_t core)
{
    return &s_tail[core];
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
static bool log_uart_clock_init(void)
{
    const uint32_t peri_hz = Cy_SysClk_ClkPeriGetFrequency();
    const uint32_t target  = LOG_UART_BAUD * LOG_UART_OVERSAMPLE;
    uint32_t       divider;
    uint32_t       actual;
    uint32_t       err_pm;

    if ((peri_hz == 0U) || (target == 0U)) {
        return false;
    }

    /* Round to nearest rather than truncating — truncation biases the achieved
     * baud high and can push an otherwise-fine rate outside tolerance. */
    divider = (peri_hz + (target / 2U)) / target;
    if (divider == 0U) {
        divider = 1U;
    }

    actual = peri_hz / divider;
    err_pm = (actual > target) ? (((actual - target) * 1000U) / target)
                               : (((target - actual) * 1000U) / target);
    if (err_pm > LOG_BAUD_TOLERANCE_PM) {
        return false;
    }

    /* PeriphSetDivider takes divider-1 ("divide by N+1"). */
    (void)Cy_SysClk_PeriphAssignDivider(LOG_UART_PCLK, LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);
    (void)Cy_SysClk_PeriphSetDivider(LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM, divider - 1U);
    (void)Cy_SysClk_PeriphEnableDivider(LOG_UART_DIV_TYPE, LOG_UART_DIV_NUM);
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
        /* No usable divider. Leave the sink down: log_evt() keeps queueing and
         * dropping harmlessly (REQ-LOG-005), and nothing else in the app is
         * affected. Silence is the correct failure mode for a log channel —
         * it must never take the application down with it. */
        return;
    }

    log_uart_pins_init();

    if (Cy_SCB_UART_Init(LOG_UART_SCB, &uart_cfg, NULL) != CY_SCB_UART_SUCCESS) {
        return;
    }
    Cy_SCB_UART_Enable(LOG_UART_SCB);

    s_sink_ready = true;

    /* Ring + banner. Producers may run before this; their records simply queue,
     * and overflow drops the NEWEST so the earliest survive (REQ-LOG-006). */
    log_init(LOG_CORE_APP);
}
