/*
 * port_crypto.c — CM4 binding of ipc_port_if_t to the TRAVEO IPC hardware
 * (M4 Seam 1, ADR-0018). The host-tested transport (shared/crypto/
 * ipc_mailbox.c, ipc_transact) drives these function pointers; this file is
 * the "real semaphore/channel" the host fake stood in for.
 *
 * Mapping (one IPC channel does all three roles ADR-0018 D1 asks for):
 *   sema_try_acquire/release -> the channel's HARDWARE LOCK (Cy_IPC_Drv_Lock*),
 *                               held by the CM4 for the whole transaction as
 *                               the single-outstanding mutex (ADR-0018 D3).
 *                               The CM0+ never touches the lock — it only
 *                               reads/writes the mailbox and flips status.
 *   notify(len)              -> write len + REQUEST into the shared mailbox,
 *                               then Cy_IPC_Drv_AcquireNotify the CM0+ doorbell.
 *   response_ready/len       -> poll the mailbox status/len the CM0+ set.
 *
 * UNVALIDATED on silicon (Seam 1 bench): the channel-not-taken-by-CyPipe
 * assumption, the notify interrupt-structure mask, and cross-core access to
 * the pinned mailbox region.
 */
#include "fbl_crypto.h"
#include "ipc_mailbox_map.h"
#include "crypto_msg.h"      /* crypto_msg_t, encode/decode (shared/crypto) */
#include "crypto_service.h"  /* crypto_service_init, crypto_verify_image */
#include "fbl_port.h"        /* fbl_port_now_ms */
#include "cy_pdl.h"          /* Cy_IPC_Drv_*, __DMB */
#include <string.h>          /* memcpy */

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
    /* Cast away volatile for the byte-buffer view the transport memcpys into;
     * the handshake fields (status/len) stay volatile and are touched only
     * through IPC_MAILBOX below. */
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

static const ipc_port_if_t g_crypto_port = {
    .sema_try_acquire = port_sema_try_acquire,
    .sema_release     = port_sema_release,
    .mailbox          = port_mailbox,
    .mailbox_cap      = CRYPTO_MAX_PAYLOAD,
    .notify           = port_notify,
    .response_ready   = port_response_ready,
    .response_len     = port_response_len,
    .now_ms           = fbl_port_now_ms
};

void fbl_crypto_port_init(void)
{
    IPC_MAILBOX->status = (uint32_t)IPC_MBX_IDLE;
    IPC_MAILBOX->len = 0U;
    __DMB();
}

const ipc_port_if_t *fbl_crypto_port(void)
{
    return &g_crypto_port;
}

/* fbl_crypto_bringup_echo() (Seam 1) was retired here: it proved the raw
 * transport against a CM0+ echo loop, but the CM0+ now runs crypto_dispatch
 * (Seam 2), so a raw pattern decodes as a malformed envelope -> ERROR verdict,
 * never an echo. The hash bring-up below exercises the same transport through
 * a real request, so nothing is lost. (History: proven true on silicon before
 * the Seam-2 swap; see M4-bringup-plan.md Seam 1.) */

bool fbl_crypto_bringup_hash(void)
{
    fbl_crypto_port_init();

    /* NIST SHA-256("abc") = ba7816bf...f20015ad. */
    static const uint8_t message[3] = { 'a', 'b', 'c' };
    static const uint8_t expected[CRYPTO_SHA256_DIGEST_LEN] = {
        0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU,
        0x41U, 0x41U, 0x40U, 0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U,
        0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U, 0x7AU, 0x9CU,
        0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU
    };

    /* Build + encode a HASH request envelope. */
    crypto_msg_t req = { 0 };
    req.op_code = (uint8_t)CRYPTO_OP_HASH;
    req.length = (uint16_t)sizeof message;
    (void)memcpy(req.payload, message, sizeof message);

    uint8_t enc[CRYPTO_MSG_MAX_WIRE];
    size_t enc_len = crypto_msg_encode(&req, enc, sizeof enc);
    if (enc_len == 0U)
    {
        return false;
    }

    uint8_t resp[CRYPTO_MSG_MAX_WIRE];
    size_t resp_len = 0U;
    if (ipc_transact(fbl_crypto_port(), enc, enc_len,
                     resp, sizeof resp, &resp_len, 1000U) != IPC_OK)
    {
        return false;
    }

    crypto_msg_t respm;
    if (!crypto_msg_decode(resp, resp_len, &respm))
    {
        return false;
    }
    if ((respm.op_code != (uint8_t)CRYPTO_OP_HASH) ||
        (respm.length != (uint16_t)CRYPTO_SHA256_DIGEST_LEN))
    {
        return false;
    }
    for (size_t i = 0U; i < (size_t)CRYPTO_SHA256_DIGEST_LEN; ++i)
    {
        if (respm.payload[i] != expected[i])
        {
            return false;
        }
    }
    return true;
}

crypto_verdict_t fbl_crypto_bringup_verify(uint32_t base, uint32_t len, uint32_t key_id)
{
    fbl_crypto_port_init();
    crypto_service_init(fbl_crypto_port());
    /* The real FBL-side client (ADR-0016) — Seam 4 calls this same path from
     * the boot decision. Any transport failure collapses to ERROR (fail-safe,
     * ADR-0016 D5); a mismatched/tampered image comes back INVALID. */
    return crypto_verify_image(base, len, key_id);
}
