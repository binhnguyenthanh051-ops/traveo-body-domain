/*
 * log_task.c — Node B CM7 log drain task (ADR-0023 D10, REQ-LOG-014).
 *
 * Output is CYCLIC, not immediate: log_evt() enqueues and returns, and this
 * task moves bytes to the UART. Immediate output would make the producer wait
 * on a ~10 us/byte UART on a per-frame path, at any priority including inside
 * an ISR — the inversion D7 exists to forbid.
 *
 * Buffering costs freshness, not truth: ts_ms is sampled inside log_evt(), at
 * production time, so drain delay never distorts the recorded timeline.
 *
 * What it DOES cost is latency, and that has to be bounded and published,
 * because the BVT asserts on the ABSENCE of events (replay/forgery) and
 * "absent" is otherwise indistinguishable from "not drained yet". The period
 * below must stay <= LOG_DRAIN_LATENCY_MS.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "tb_log.h"
#include "log_types.h"

/* Lowest priority: logging must never delay CAN or the actuator FSM. If this
 * task is starved, producers degrade to dropping (counted, and reported via
 * LOG_EVT_OVERFLOW) and nothing stalls. tskIDLE_PRIORITY + 1 puts it level with
 * the heartbeat and below everything that does work.
 *
 * The period must stay <= LOG_DRAIN_LATENCY_MS: it IS the published bound a BVT
 * absence-assertion waits out before concluding an event never happened — and
 * on THIS node those assertions are the SecOC replay/forgery tests. */
#define LOG_TASK_PRIO       (tskIDLE_PRIORITY + 1U)
#define LOG_TASK_PERIOD_MS  20U
#define LOG_TASK_STACK      configMINIMAL_STACK_SIZE

/* Bytes moved per wake. Bounds how long one drain can hold the core: at
 * 1 Mbps the FIFO drains ~100 B/ms, so 256 B is well under a tick even if the
 * sink accepts everything at once. The remainder simply waits for the next
 * wake — which is why the period, not this number, sets the latency bound. */
#define LOG_TASK_MAX_BYTES  256U

static StaticTask_t s_log_tcb;
static StackType_t  s_log_stack[LOG_TASK_STACK];

static void log_cyclic_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    (void)arg;

    for (;;) {
        /* Drain until the ring is empty or the sink pushes back. A single
         * bounded call per wake would leave a backlog growing under burst;
         * looping while progress is being made clears it within the period. */
        log_drain_t st = log_drain(LOG_TASK_MAX_BYTES);

        while (st == LOG_DRAIN_SENT) {
            st = log_drain(LOG_TASK_MAX_BYTES);
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(LOG_TASK_PERIOD_MS));
    }
}

void log_task_create(void)
{
    TaskHandle_t h = xTaskCreateStatic(log_cyclic_task,
                                       "LOG",
                                       LOG_TASK_STACK,
                                       NULL,
                                       LOG_TASK_PRIO,
                                       s_log_stack,
                                       &s_log_tcb);
    configASSERT(h != NULL);
    (void)h;
}
