/*
 * app_task.c — App_CyclicTask (ADR-0010 D5/D6).
 *
 * Target-only. Owns app_msg_queue(): receives decoded body_msg_t (event-fed,
 * with a period timeout), steps the host-testable body-control state machine,
 * and acts on its outputs. The state machine never sees the queue handle (the
 * M2-8 seam) — this task is the only adapter between them.
 *
 * Also polls the user button: a press issues the App-side programming-request
 * (the M2 debug trigger, ADR-0007 D7), which writes .noinit and resets.
 *
 * Stub: actuator outputs and the button GPIO are marked TODO until bring-up.
 */
#include "tasks.h"
#include "task.h"
#include "bodyctl.h"
#include "reprogram.h"
#include "body_msgs.h"

#define APP_STACK_WORDS     512U   /* generous for bring-up; trim from high-water (ADR-0010 D5) */
#define APP_MSG_QDEPTH       8U

static StaticTask_t  s_tcb;
static StackType_t   s_stack[APP_STACK_WORDS];

static StaticQueue_t s_msg_q_ctrl;
static uint8_t       s_msg_q_store[APP_MSG_QDEPTH * sizeof(body_msg_t)];
static QueueHandle_t s_msg_q;

static bodyctl_state_t s_body;

/* Seam-3 reprogram trigger (M2 debug trigger, ADR-0007 D7). Set to 1 in the
 * debugger to request reprogramming — no GPIO needed. A physical user button can
 * drive this later (set APP_BTN_PORT/PIN and poll it in the cyclic slot). */
volatile uint32_t g_reprogram_request;

QueueHandle_t app_msg_queue(void) { return s_msg_q; }

static void apply_output(const bodyctl_output_t *out)
{
    if (out->light_cmd_valid)
    {
        /* TODO(bring-up): drive the light output / LED to out->light_pct. */
        (void)out->light_pct;
    }
    if (out->lock_cmd_valid)
    {
        /* TODO(bring-up): drive the lock actuator to out->lock_locked. */
        (void)out->lock_locked;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    body_msg_t       msg;
    bodyctl_output_t out;

    bodyctl_init(&s_body);

    for (;;)
    {
        if (xQueueReceive(s_msg_q, &msg, pdMS_TO_TICKS(APP_PERIOD_APP_MS)) == pdTRUE)
        {
            bodyctl_step(&s_body, &msg, &out);
            apply_output(&out);
        }

        /* Cyclic slot: poll the seam-3 reprogram trigger. app_request_reprogram()
         * writes the PROGRAMMING_REQUESTED handshake into .noinit and triggers a
         * software reset (does not return); the FBL then reads it and stays in
         * programming mode, clearing the boot-loop counter (ADR-0007 D7/D9). */
        if (g_reprogram_request != 0U)
        {
            app_request_reprogram();
        }
    }
}

void app_task_create(void)
{
    s_msg_q = xQueueCreateStatic(APP_MSG_QDEPTH, sizeof(body_msg_t),
                                 s_msg_q_store, &s_msg_q_ctrl);
    configASSERT(s_msg_q != NULL);

    TaskHandle_t h = xTaskCreateStatic(app_task, "app", APP_STACK_WORDS,
                                       NULL, APP_PRIO_APP, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
