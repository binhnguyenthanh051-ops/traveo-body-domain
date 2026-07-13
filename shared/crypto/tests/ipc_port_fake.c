/*
 * ipc_port_fake.c — host fake for ipc_port_if_t (ADR-0018), a scriptable
 * "loopback M0+". Test harness — exempt from MISRA.
 *
 * Scripts the other side of the mailbox: a canned response delivered on
 * notify(), a denied semaphore (BUSY), or silence (TIMEOUT). now_ms()
 * auto-advances a fixed step per call so a bounded-wait loop terminates
 * without any real clock — the same fake-clock idea as the boot dwell tests.
 */
#include "ipc_mailbox.h"
#include <string.h>

#define FAKE_MAILBOX_CAP   256U

static uint8_t g_mailbox[FAKE_MAILBOX_CAP];

static bool     g_sema_grant;       /* sema_try_acquire returns this */
static bool     g_held;             /* currently acquired? (catches release bugs) */
static bool     g_deliver;          /* does notify() arm a response? */
static uint8_t  g_response[FAKE_MAILBOX_CAP];
static size_t   g_response_len;
static bool     g_ready;            /* response_ready() flag, set by notify() */
static uint32_t g_now;
static uint32_t g_step;

static int      g_notify_count;
static uint8_t  g_last_request[FAKE_MAILBOX_CAP];
static size_t   g_last_request_len;

void fake_ipc_reset(void)
{
    memset(g_mailbox, 0, sizeof g_mailbox);
    memset(g_response, 0, sizeof g_response);
    memset(g_last_request, 0, sizeof g_last_request);
    g_sema_grant = true;
    g_held = false;
    g_deliver = false;
    g_response_len = 0U;
    g_ready = false;
    g_now = 0U;
    g_step = 1U;
    g_notify_count = 0;
    g_last_request_len = 0U;
}

/* Arm a canned response envelope to be delivered on the next notify(). */
void fake_ipc_script_response(const uint8_t *resp, size_t len)
{
    if (len > sizeof g_response) { len = sizeof g_response; }
    memcpy(g_response, resp, len);
    g_response_len = len;
    g_deliver = true;
}

/* The M0+ never answers — notify() arms nothing, response_ready stays false. */
void fake_ipc_script_no_response(void) { g_deliver = false; }

/* The mailbox semaphore is contended — sema_try_acquire fails. */
void fake_ipc_script_deny_sema(void) { g_sema_grant = false; }

void fake_ipc_set_step(uint32_t step_ms) { g_step = step_ms; }

int  fake_ipc_notify_count(void) { return g_notify_count; }
bool fake_ipc_sema_held(void)    { return g_held; }

const uint8_t *fake_ipc_last_request(size_t *out_len)
{
    if (out_len != NULL) { *out_len = g_last_request_len; }
    return g_last_request;
}

/* ---- ipc_port_if_t implementation ---- */

static bool fake_sema_try_acquire(void)
{
    if (!g_sema_grant) { return false; }
    g_held = true;
    return true;
}

static void fake_sema_release(void) { g_held = false; }

static uint8_t *fake_mailbox(void) { return g_mailbox; }

static void fake_notify(size_t req_len)
{
    ++g_notify_count;
    /* Capture exactly the request bytes the client wrote. */
    if (req_len > sizeof g_last_request) { req_len = sizeof g_last_request; }
    memcpy(g_last_request, g_mailbox, req_len);
    g_last_request_len = req_len;

    if (g_deliver)
    {
        memcpy(g_mailbox, g_response, g_response_len);
        g_ready = true;
    }
}

static bool fake_response_ready(void) { return g_ready; }

static size_t fake_response_len(void) { return g_response_len; }

static uint32_t fake_now_ms(void)
{
    uint32_t t = g_now;
    g_now += g_step;
    return t;
}

static const ipc_port_if_t g_fake_port = {
    .sema_try_acquire = fake_sema_try_acquire,
    .sema_release     = fake_sema_release,
    .mailbox          = fake_mailbox,
    .mailbox_cap      = FAKE_MAILBOX_CAP,
    .notify           = fake_notify,
    .response_ready   = fake_response_ready,
    .response_len     = fake_response_len,
    .now_ms           = fake_now_ms
};

const ipc_port_if_t *fake_ipc_port(void) { return &g_fake_port; }
