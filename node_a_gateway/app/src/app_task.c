/*
 * app_task.c — App_CyclicTask (ADR-0010 D5/D6).
 *
 * Target-only. Owns app_msg_queue(): receives decoded body_msg_t (event-fed,
 * with a period timeout), steps the host-testable body-control state machine,
 * and acts on its outputs. The state machine never sees the queue handle (the
 * M2-8 seam) — this task is the only adapter between them.
 *
 * Also polls the user button: a press issues the App-side programming-request
 * (the M2 trigger, ADR-0007 D7), which writes .noinit and resets.
 *
 * The actuator outputs are still TODO(bring-up); the user button is wired.
 */
#include "tasks.h"
#include "task.h"
#include "bodyctl.h"
#include "reprogram.h"
#include "body_msgs.h"
#include "secoc_app.h"   /* secured command TX (M5 seam 3) */
#include "cy_pdl.h"   /* Cy_GPIO_* */

#define APP_STACK_WORDS     192U   /* used ~34 words (g_hw_app) + margin for the reprogram path (ADR-0010 D5) */
#define APP_MSG_QDEPTH       8U

/* User button SW1 = P7.0 on the CYTVII-B-E-1M-SK, active-low (idle high via the
 * internal pull-up; a press pulls it to GND). A debounced press drives the same
 * reprogram request as the debug flag. */
#define APP_BTN_PORT         GPIO_PRT7
#define APP_BTN_PIN          0U
#define APP_BTN_DEBOUNCE     3U     /* consecutive low polls (~3 x APP_PERIOD_APP_MS) */

static StaticTask_t  s_tcb;
static StackType_t   s_stack[APP_STACK_WORDS];

static StaticQueue_t s_msg_q_ctrl;
static uint8_t       s_msg_q_store[APP_MSG_QDEPTH * sizeof(body_msg_t)];
static QueueHandle_t s_msg_q;

static bodyctl_state_t s_body;

/* Seam-3 reprogram trigger (ADR-0007 D7): raised by the debounced user button
 * (below) OR set to 1 by hand in the debugger. */
volatile uint32_t g_reprogram_request;

/* Bring-up: free stack words (min ever) for this task — read in the debugger to
 * size the stack (ADR-0010 D5). */
volatile UBaseType_t g_hw_app;

QueueHandle_t app_msg_queue(void) { return s_msg_q; }

/* Debounce SW1 and raise g_reprogram_request on a clean press edge. */
static void poll_reprogram_button(void)
{
    static uint32_t low_count = 0U;
    static bool     pressed   = false;

    if (Cy_GPIO_Read(APP_BTN_PORT, APP_BTN_PIN) == 0U)   /* active-low */
    {
        if (low_count < APP_BTN_DEBOUNCE) { ++low_count; }
    }
    else
    {
        low_count = 0U;
    }

    bool stable = (low_count >= APP_BTN_DEBOUNCE);
    if (stable && !pressed)
    {
        g_reprogram_request = 1U;   /* rising edge of a debounced press */
    }
    pressed = stable;
}

static void apply_output(const bodyctl_output_t *out)
{
#if !CAN_LOOPBACK_TEST
    /* Phase B (M5): the gateway does not actuate locally — it SENDS authenticated
     * commands to the actuator (Node B), which verifies and drives the hardware.
     * secoc_app_build_* secures the frame (freshness + MAC via the M0+); a build
     * failure (freshness/oracle) simply skips this command — the actuator holds
     * its last safe state. */
    can_raw_frame_t f;
    if (out->lock_cmd_valid && secoc_app_build_door_cmd(out->lock_locked, &f))
    {
        (void)can_app_send(&f);
    }
    if (out->light_cmd_valid && secoc_app_build_light_cmd(out->light_pct, &f))
    {
        (void)can_app_send(&f);
    }
#else
    /* Phase A: local outputs (TODO bring-up), no bus/SecOC. */
    if (out->light_cmd_valid) { (void)out->light_pct; }
    if (out->lock_cmd_valid)  { (void)out->lock_locked; }
#endif
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

        g_hw_app = uxTaskGetStackHighWaterMark(NULL);

        /* Cyclic slot: poll the seam-3 reprogram trigger (button or debug flag).
         * app_request_reprogram() writes the PROGRAMMING_REQUESTED handshake into
         * .noinit and triggers a software reset (does not return); the FBL reads it
         * and stays in programming mode, clearing the boot-loop counter (D7/D9). */
        poll_reprogram_button();
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

    /* SW1 / P7.0 as an input with the internal pull-up (active-low). */
    Cy_GPIO_Pin_FastInit(APP_BTN_PORT, APP_BTN_PIN, CY_GPIO_DM_PULLUP, 1U, HSIOM_SEL_GPIO);

    TaskHandle_t h = xTaskCreateStatic(app_task, "app", APP_STACK_WORDS,
                                       NULL, APP_PRIO_APP, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
