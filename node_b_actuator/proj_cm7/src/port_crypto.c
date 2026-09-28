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
#include "tb_log.h"            /* log_evt */
#include "log_events.h"        /* LOG_EVT_DBG_U32 */
#include <string.h>            /* memcmp */

/* On-silicon offload round-trip check (call from a task, after the scheduler is
 * running and the CM0+ server is up). Proves the CM7<->CM0+ MAC path end to end
 * WITHOUT a precomputed tag: same input => same tag (deterministic), a changed
 * input => a changed tag, and an unknown key_id => failure. The AES-CMAC
 * primitive's NIST SP 800-38B KAT is a separate check on the CM0+ (Stage 2).
 *
 * Does NOT call secoc_crypto_port_init(): main.c already did, before the
 * scheduler, and re-running it from a task would idle the mailbox under a
 * transaction another task may have in flight.
 *
 * Returns 0 on pass, else the number of the first check that failed. */
uint32_t secoc_crypto_bringup_mac(void)
{
    const uint8_t msg_a[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    const uint8_t msg_b[4] = { 0x11U, 0x22U, 0x33U, 0x45U };   /* one byte different */
    uint8_t t1[CRYPTO_CMAC_TAG_LEN];
    uint8_t t2[CRYPTO_CMAC_TAG_LEN];
    uint8_t t3[CRYPTO_CMAC_TAG_LEN];
    uint8_t t4[CRYPTO_CMAC_TAG_LEN];

    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_a, sizeof msg_a, t1)) { return 1U; }  /* no answer / error */
    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_a, sizeof msg_a, t2)) { return 2U; }
    if (memcmp(t1, t2, sizeof t1) != 0) { return 3U; }      /* not deterministic */

    if (!crypto_mac(SECOC_MAC_KEY_ID, msg_b, sizeof msg_b, t3)) { return 4U; }
    if (memcmp(t1, t3, sizeof t1) == 0) { return 5U; }      /* not input-sensitive */

    if (crypto_mac(0x99U, msg_a, sizeof msg_a, t4)) { return 6U; }  /* unknown key accepted */

    return 0U;
}

/* Bench Stage 3.3 + 3.4 in one go, reported on the log channel so no CM7 debug
 * session is needed. All records are LOG_EVT_DBG_U32 (value, tag):
 *   tag 0x3300  value 0           round trip starting (no 0x3303 after it = hang)
 *   tag 0x3303  value result      0 = pass, else the failed check (see above)
 *   tag 0x3340  value SCB->CCR    bit 16 (DC) = D-cache on, bit 17 (IC) = I-cache on
 *   tag 0x3341  value MPU->CTRL   bit 0 = MPU on, bit 2 = PRIVDEFENA
 *   tag 0x335r  value RBAR        region r (enabled regions only)
 *   tag 0x336r  value RASR        region r (enabled regions only) */
void secoc_crypto_bringup_report(void)
{
    log_evt(LOG_EVT_DBG_U32, 0U, 0x3300U);
    log_evt(LOG_EVT_DBG_U32, secoc_crypto_bringup_mac(), 0x3303U);

    log_evt(LOG_EVT_DBG_U32, SCB->CCR, 0x3340U);
    log_evt(LOG_EVT_DBG_U32, MPU->CTRL, 0x3341U);

    /* MPU->TYPE can't be spelled here: the PDL's cy_crypto_common.h does
     * `#define TYPE uint8_t`. TYPE is the first MPU register (offset 0). */
    const uint32_t mpu_type = *(volatile const uint32_t *)(uintptr_t)MPU_BASE;
    const uint32_t regions  = (mpu_type & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
    for (uint32_t r = 0U; r < regions; r++)
    {
        MPU->RNR = r;
        const uint32_t rasr = MPU->RASR;
        if ((rasr & MPU_RASR_ENABLE_Msk) != 0UL)
        {
            log_evt(LOG_EVT_DBG_U32, MPU->RBAR, (uint16_t)(0x3350U + r));
            log_evt(LOG_EVT_DBG_U32, rasr, (uint16_t)(0x3360U + r));
        }
    }
}
#endif /* SECOC_CRYPTO_BRINGUP */
