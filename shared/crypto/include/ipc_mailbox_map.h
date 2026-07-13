/*
 * ipc_mailbox_map.h — the physical CM4<->CM0+ mailbox binding (M4 Seam 1,
 * ADR-0018 D1/D5). Shared by BOTH target images (the CM4 FBL port and the
 * CM0+ crypto service) so they can never disagree on the address or layout —
 * the same single-source discipline as noinit.ld for the FBL<->app handshake.
 *
 * This header carries only fixed addresses/constants and a POD layout — no
 * vendor headers — so it does not violate the "portable logic includes no
 * vendor headers" rule (ADR-0001). The portable transport (ipc_mailbox.c)
 * does NOT include this; only the two target-side bindings do.
 *
 * UNVALIDATED on silicon — the address, the channel-not-taken-by-CyPipe
 * assumption, and cross-core access to the region are Seam-1 bench checks
 * (see docs/briefs/M4-bringup-plan.md).
 */
#ifndef IPC_MAILBOX_MAP_H
#define IPC_MAILBOX_MAP_H

#include <stdint.h>
#include "crypto_types.h"   /* CRYPTO_MAX_PAYLOAD */

/* Fixed shared-SRAM address of the mailbox (ADR-0018 D5). It lives in a pinned
 * region just below .noinit in CM4's RAM partition, reserved by
 * proj_cm4/linker/fbl_cm4.ld (_ipc_mailbox_start) so CM4 .data/.bss/stack
 * avoid it. The CM0+ image — whose linker only allocates the 0x0800_0000..8000
 * tenant — reaches it by absolute address; nothing in CM0+ startup zeroes it.
 * MUST equal _ipc_mailbox_start in fbl_cm4.ld. */
#define IPC_MAILBOX_ADDR     0x0801F500UL

/* IPC channel carrying the doorbell + owning the transaction lock (ADR-0018
 * D1). CY_IPC_CHAN_USER == 4 on CYT2B7 (channels 0-3 = SYSCALL_CM0 / SYSCALL_
 * CM4 / SYSCALL_DAP / SEMA; see cy_device.h COMPONENT_CAT1A). The channel's
 * hardware lock is the mailbox-ownership semaphore (CLAUDE.md HW-semaphore
 * mandate). VERIFY on bench that CyPipe/DDFT is not initialised over it. */
#define IPC_CRYPTO_CHANNEL   4U

/* IPC interrupt-structure mask AcquireNotify rings to wake the CM0+ (the
 * doorbell). At Seam 1 the CM0+ POLLS the mailbox status instead of taking an
 * ISR, so this is best-effort until the notify ISR is wired; the exact
 * structure/mask is a bench item. */
#define IPC_CRYPTO_NOTIFY_INTR_MASK   (1UL << IPC_CRYPTO_CHANNEL)

/* Transport-level handshake (opaque to crypto_msg): the initiator sets len +
 * status=REQUEST; the server sets len + status=RESPONSE. status/len are the
 * ADR-0018 "length beside the doorbell", payload carries the crypto envelope. */
typedef enum {
    IPC_MBX_IDLE     = 0U,
    IPC_MBX_REQUEST  = 1U,
    IPC_MBX_RESPONSE = 2U
} ipc_mbx_status_t;

typedef struct {
    volatile uint32_t status;              /* ipc_mbx_status_t */
    volatile uint32_t len;                 /* request or response byte count */
    uint8_t           payload[CRYPTO_MAX_PAYLOAD];
} ipc_mailbox_shared_t;

/* Both cores map the region here. */
#define IPC_MAILBOX   ((volatile ipc_mailbox_shared_t *)IPC_MAILBOX_ADDR)

#endif /* IPC_MAILBOX_MAP_H */
