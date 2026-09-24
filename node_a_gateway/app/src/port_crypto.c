/*
 * port_crypto.c — Node A APP binding of ipc_port_if_t to the TRAVEO IPC hardware
 * (M5). Mirrors the FBL's proven bootloader/proj_cm4/src/port_crypto.c, with an
 * app-side (FreeRTOS-tick) clock and no bring-up self-tests. The M4 has no cache,
 * so — unlike Node B's CM7 binding — there is no MPU non-cacheable step.
 *
 * Uses Node A's mailbox map (shared/crypto/include/ipc_mailbox_map.h: IPC_MAILBOX
 * at 0x0801F500, IPC_CRYPTO_CHANNEL = 4 on CYT2B7/CAT1A). The FBL and the app are
 * two CM4 images that never run simultaneously, so they share the one mailbox
 * without contention.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "ipc_mailbox_map.h"   /* IPC_MAILBOX, IPC_CRYPTO_CHANNEL, ipc_mbx_status_t */
#include "ipc_mailbox.h"       /* ipc_port_if_t */
#include "crypto_service.h"    /* crypto_service_init, crypto_mac */
#include "crypto_types.h"      /* CRYPTO_MAX_PAYLOAD */
#include "port_crypto.h"
#include "cy_pdl.h"            /* Cy_IPC_Drv_*, __DMB */

static IPC_STRUCT_Type *ipc_ch(void)
{
    return Cy_IPC_Drv_GetIpcBaseAddress(IPC_CRYPTO_CHANNEL);
}

static bool port_sema_try_acquire(void)
{
    return (Cy_IPC_Drv_LockAcquire(ipc_ch()) == CY_IPC_DRV_SUCCESS);
}

static void port_sema_release(void)
{
    IPC_MAILBOX->status = (uint32_t)IPC_MBX_IDLE;
    (void)Cy_IPC_Drv_LockRelease(ipc_ch(), CY_IPC_NO_NOTIFICATION);
}

static uint8_t *port_mailbox(void)
{
    return (uint8_t *)(uintptr_t)IPC_MAILBOX->payload;
}

static void port_notify(size_t req_len)
{
    IPC_MAILBOX->len = (uint32_t)req_len;
    __DMB();                                  /* payload + len land before the doorbell */
    IPC_MAILBOX->status = (uint32_t)IPC_MBX_REQUEST;
    (void)Cy_IPC_Drv_AcquireNotify(ipc_ch(), IPC_CRYPTO_NOTIFY_INTR_MASK);
}

static bool port_response_ready(void)
{
    return (IPC_MAILBOX->status == (uint32_t)IPC_MBX_RESPONSE);
}

static size_t port_response_len(void)
{
    return (size_t)IPC_MAILBOX->len;
}

/* FreeRTOS tick as the bounded-poll clock. Task context only (after the
 * scheduler starts) — every crypto_mac() call runs from a task. */
static uint32_t port_now_ms(void)
{
    return (uint32_t)((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS);
}

static const ipc_port_if_t g_crypto_port = {
    .sema_try_acquire = port_sema_try_acquire,
    .sema_release     = port_sema_release,
    .mailbox          = port_mailbox,
    .mailbox_cap      = CRYPTO_MAX_PAYLOAD,
    .notify           = port_notify,
    .response_ready   = port_response_ready,
    .response_len     = port_response_len,
    .now_ms           = port_now_ms
};

void secoc_crypto_port_init(void)
{
    IPC_MAILBOX->status = (uint32_t)IPC_MBX_IDLE;
    IPC_MAILBOX->len = 0U;
    __DMB();
    crypto_service_init(&g_crypto_port);
}

const ipc_port_if_t *secoc_crypto_port(void)
{
    return &g_crypto_port;
}
