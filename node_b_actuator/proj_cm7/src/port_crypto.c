/*
 * port_crypto.c — Node B CM7 binding of ipc_port_if_t to the TRAVEO IPC hardware
 * (M5 crypto seam, ADR-0018). The host-tested transport (shared/crypto/
 * ipc_mailbox.c, ipc_transact) drives these function pointers; this file is the
 * "real semaphore/channel" the host fake stood in for. It is the CLIENT side —
 * the CM0+ is the server (node_b_actuator/proj_cm0p/src/main_cm0p.c).
 *
 * Mirrors Node A's port_crypto.c, with three deltas:
 *   - the mailbox is IPC_MAILBOX_B (Node B map, in the non-cacheable window);
 *   - now_ms rides the FreeRTOS tick (Node A used the FBL clock);
 *   - it programs the MPU non-cacheable region (ADR-0018 D6) — Node A's M4 has no
 *     cache, so this is genuinely new. Without it, on a D-cache-enabled CM7 the
 *     mailbox is cacheable and the cross-core handshake is a silent coherency
 *     bug: a CM7 write can sit in the D-cache invisible to the CM0+, a CM7 read
 *     can return a stale line.
 *
 * Mapping (one IPC channel does all three ADR-0018 D1 roles):
 *   sema_try_acquire/release -> the channel's HARDWARE LOCK (Cy_IPC_Drv_Lock*),
 *                               held for the whole transaction (single-outstanding
 *                               mutex, ADR-0018 D3). The CM0+ never takes the lock.
 *   notify(len)              -> write len + REQUEST into the mailbox, then ring
 *                               the CM0+ doorbell. (The CM0+ server POLLS status,
 *                               so the notify is best-effort until its ISR lands.)
 *   response_ready/len       -> poll the status/len the CM0+ server set.
 *
 * UNVALIDATED on silicon (crypto-seam bench). VERIFY items are flagged inline.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "ipc_mailbox_map.h"   /* IPC_MAILBOX_B, ipc_mbx_status_t (Node B map) */
#include "ipc_mailbox.h"       /* ipc_port_if_t (shared/crypto) */
#include "crypto_service.h"    /* crypto_service_init, crypto_mac */
#include "crypto_types.h"      /* CRYPTO_MAX_PAYLOAD, CRYPTO_CMAC_TAG_LEN */
#include "port_crypto.h"
#include "cy_pdl.h"            /* Cy_IPC_Drv_*, ARM_MPU_*, __DMB/__DSB/__ISB */

/* CAT1C user IPC channel for the SecOC mailbox (ADR-0018 D1). CY_IPC_CHAN_USER is
 * the first channel not reserved by SYSCALL/DAP/CyPipe on this family — reference
 * the PDL rather than hardcode an index. VERIFY (bench): this channel is not
 * claimed by CyPipe/DDFT. */
#define IPC_CRYPTO_CHANNEL_B      (CY_IPC_CHAN_USER)
#define IPC_CRYPTO_NOTIFY_MASK    (1UL << IPC_CRYPTO_CHANNEL_B)

/* The non-cacheable window (must match the COMPONENT_CM7 linker's ram_noncache /
 * Cy_SecOc_IpcMailbox base — ADR-0018 D6). A high MPU region index so we don't
 * clobber a region the BSP may already program. */
#define SECOC_NONCACHE_BASE       0x280E0000UL
#define SECOC_NONCACHE_SIZE       ARM_MPU_REGION_SIZE_128KB
#define SECOC_MPU_REGION          7UL

static IPC_STRUCT_Type *ipc_ch(void)
{
    return Cy_IPC_Drv_GetIpcBaseAddress(IPC_CRYPTO_CHANNEL_B);
}

static bool port_sema_try_acquire(void)
{
    return (Cy_IPC_Drv_LockAcquire(ipc_ch()) == CY_IPC_DRV_SUCCESS);
}

static void port_sema_release(void)
{
    IPC_MAILBOX_B->status = (uint32_t)IPC_MBX_IDLE;
    (void)Cy_IPC_Drv_LockRelease(ipc_ch(), CY_IPC_NO_NOTIFICATION);
}

static uint8_t *port_mailbox(void)
{
    return (uint8_t *)(uintptr_t)IPC_MAILBOX_B->payload;
}

static void port_notify(size_t req_len)
{
    IPC_MAILBOX_B->len = (uint32_t)req_len;
    __DMB();                                  /* payload + len land before the doorbell */
    IPC_MAILBOX_B->status = (uint32_t)IPC_MBX_REQUEST;
    (void)Cy_IPC_Drv_AcquireNotify(ipc_ch(), IPC_CRYPTO_NOTIFY_MASK);
}

static bool port_response_ready(void)
{
    return (IPC_MAILBOX_B->status == (uint32_t)IPC_MBX_RESPONSE);
}

static size_t port_response_len(void)
{
    return (size_t)IPC_MAILBOX_B->len;
}

/* FreeRTOS tick as the millisecond clock the bounded-poll timeout uses. MUST be
 * called from task context (after the scheduler starts) — xTaskGetTickCount is 0
 * before then, which would defeat the timeout. */
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

/* Program one MPU region marking the SecOC non-cacheable window Normal,
 * non-cacheable (TEX=1,C=0,B=0), shareable, execute-never (ADR-0018 D6). Adds a
 * single region at a high index and only enables the MPU (with the background
 * default map) if it is not already on, so it composes with a BSP that already
 * uses the MPU.
 *
 * VERIFY (bench): (a) whether the CAT1C BSP already configures an MPU region for
 * ram_noncache — if so, reconcile rather than duplicate; (b) that SECOC_MPU_REGION
 * (7) is free; (c) that the CM7 D-cache is actually enabled (if not, everything is
 * effectively non-cacheable and this is harmless but not yet load-bearing). */
static void secoc_mpu_noncache_init(void)
{
    ARM_MPU_SetRegion(ARM_MPU_RBAR(SECOC_MPU_REGION, SECOC_NONCACHE_BASE),
                      ARM_MPU_RASR(1UL,               /* DisableExec (XN) */
                                   ARM_MPU_AP_FULL,   /* RW any privilege */
                                   1UL,               /* TEX = 1 ... */
                                   1UL,               /* S   = 1 ... */
                                   0UL,               /* C   = 0 ... */
                                   0UL,               /* B   = 0  => Normal, non-cacheable */
                                   0UL,               /* no sub-regions disabled */
                                   SECOC_NONCACHE_SIZE));
    if ((MPU->CTRL & MPU_CTRL_ENABLE_Msk) == 0UL)
    {
        ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
    }
    __DSB();
    __ISB();
}

void secoc_crypto_port_init(void)
{
    secoc_mpu_noncache_init();

    /* Idle the mailbox before the first transaction (the CM0+ server started
     * with whatever the linker left — this NOLOAD region is not zeroed). */
    IPC_MAILBOX_B->status = (uint32_t)IPC_MBX_IDLE;
    IPC_MAILBOX_B->len = 0U;
    __DMB();

    crypto_service_init(&g_crypto_port);
}

const ipc_port_if_t *secoc_crypto_port(void)
{
    return &g_crypto_port;
}

#if defined(SECOC_CRYPTO_BRINGUP) && (SECOC_CRYPTO_BRINGUP != 0)
#include "secoc_key_id.h"      /* SECOC_MAC_KEY_ID (public selector; no secret) */
#include <string.h>            /* memcmp */

/* On-silicon offload round-trip check (call from a task, after the scheduler is
 * running and the CM0+ server is up). Proves the CM7<->CM0+ MAC path end to end
 * WITHOUT a precomputed tag: same input => same tag (deterministic), a changed
 * input => a changed tag, and an unknown key_id => failure. The AES-CMAC
 * primitive's NIST SP 800-38B KAT is a separate check on the CM0+ handler. */
bool secoc_crypto_bringup_mac(void)
{
    secoc_crypto_port_init();

    const uint8_t msg_a[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    const uint8_t msg_b[4] = { 0x11U, 0x22U, 0x33U, 0x45U };   /* one byte different */
    uint8_t t1[CRYPTO_CMAC_TAG_LEN];
    uint8_t t2[CRYPTO_CMAC_TAG_LEN];
    uint8_t t3[CRYPTO_CMAC_TAG_LEN];
    uint8_t t4[CRYPTO_CMAC_TAG_LEN];

    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_a, sizeof msg_a, t1)) { return false; }
    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_a, sizeof msg_a, t2)) { return false; }
    if (memcmp(t1, t2, sizeof t1) != 0) { return false; }      /* deterministic */

    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_b, sizeof msg_b, t3)) { return false; }
    if (memcmp(t1, t3, sizeof t1) == 0) { return false; }      /* input-sensitive */

    if (crypto_mac(0x99U, msg_a, sizeof msg_a, t4)) { return false; }  /* unknown key => fail */

    return true;
}
#endif /* SECOC_CRYPTO_BRINGUP */
