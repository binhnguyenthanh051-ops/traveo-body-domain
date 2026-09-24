/*
 * ipc_mailbox_map.h (Node B) — the physical CM7<->CM0+ mailbox binding for the
 * Body High Lite part (CYT4BF, CAT1C). Node-B-specific: it must NOT be confused
 * with Node A's shared/crypto/include/ipc_mailbox_map.h, whose address
 * (0x0801F500) and IPC channel (CAT1A) are CYT2B7-only.
 *
 * Shared by BOTH Node B target images (the CM7 app's mailbox client and the
 * CM0+ crypto server) so they can never disagree on address or layout — the
 * same single-source discipline as Node A. POD + no vendor headers (ADR-0001);
 * the portable transport ipc_mailbox.c does NOT include it, only the two
 * target-side bindings do.
 *
 * ── The two ways this differs from Node A, both load-bearing ──
 * 1. ADDRESS via LINKER SYMBOL, not a literal (ADR-0018 D5 + D6). The mailbox
 *    lives in the CM7 linker's existing non-cacheable region
 *    (`_base_SRAM_NON_CACHE`), reserved in BOTH the proj_cm0p and proj_cm7
 *    linker scripts. Deferring the address to the linker is deliberate: it is
 *    the only way to guarantee the region actually lands non-cacheable (D6) —
 *    a hardcoded literal could drift out of the region on a relink. CM7 has L1
 *    D-cache, so a cacheable mailbox is a silent coherency bug (ADR-0018 M5
 *    addendum), not a theoretical one.
 * 2. IPC CHANNEL is a BRING-UP PARAMETER, not guessed here. CAT1C's channel
 *    allocation and CY_SYSINT_INTRSRC_MUXIRQ layout differ from CAT1A; the
 *    server sets the channel from the CAT1C PDL (CY_IPC_CHAN_USER + offset).
 *    See VERIFY notes below — do not fabricate a number.
 *
 * UNVALIDATED on silicon — every VERIFY item is a Node B crypto-seam bench check.
 */
#ifndef NODE_B_IPC_MAILBOX_MAP_H
#define NODE_B_IPC_MAILBOX_MAP_H

#include <stdint.h>
#include "crypto_types.h"   /* CRYPTO_MAX_PAYLOAD — identical envelope size to Node A */

/* Transport-level handshake (opaque to crypto_msg): identical semantics to Node
 * A — the initiator sets len + status=REQUEST, the server sets len +
 * status=RESPONSE. The transport code stays byte-identical across nodes; only
 * the memory attribute (non-cacheable, D6) and address (linker) differ. */
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

/* The mailbox address is supplied by the LINKER (D5/D6), not a literal.
 *
 * BOTH linker scripts must export this symbol at the SAME address inside the
 * non-cacheable region:
 *   - proj_cm7/.../COMPONENT_CM7 linker: place `.secoc_ipc_mailbox` in the
 *     region based at `_base_SRAM_NON_CACHE`, and neither zero it in startup.
 *   - proj_cm0p linker: reserve the identical address so CM0+ .data/.bss/stack
 *     avoid it (the CM0+ reaches the region by absolute address).
 *
 * VERIFY (bench): (a) the symbol resolves to the same address in both maps;
 * (b) that address sits within the MPU non-cacheable region actually programmed
 * at CM7 startup (ADR-0018 D6); (c) the region is aligned to the ARMv7-M PMSAv7
 * rule (power-of-two, >= 32 B, size-aligned) AND >= 4-byte aligned for the u32
 * `status`/`len` fields. */
extern uint8_t Cy_SecOc_IpcMailbox[];   /* [sizeof(ipc_mailbox_shared_t)] in ram_noncache */

#define IPC_MAILBOX_B \
    ((volatile ipc_mailbox_shared_t *)(void *)Cy_SecOc_IpcMailbox)

/* IPC channel + notify are BRING-UP PARAMETERS — set them in the CM0+ server /
 * CM7 client from the CAT1C PDL, do NOT hardcode here:
 *
 *   IPC channel   : a free user channel, CY_IPC_CHAN_USER-based (CAT1C reserves
 *                   the low channels for SYSCALL/DAP/CyPipe just like CAT1A, but
 *                   the count/indices differ — read cy_device.h / cy_ipc_config.h
 *                   for COMPONENT_CAT1C). The channel's HW lock is the mailbox-
 *                   ownership semaphore (CLAUDE.md HW-semaphore mandate).
 *                   VERIFY: the chosen channel is not claimed by CyPipe/DDFT.
 *   notify IRQ    : the channel's AcquireNotify wakes the CM0+ server; the exact
 *                   interrupt structure/mask and the CM0+ NVIC wiring are bench
 *                   items. Note the CAT1C NvicMux caveat already hit on Node B
 *                   (NvicMux0/1 ROM-reserved; MUXIRQ_SHIFT = 16). At first
 *                   bring-up the server MAY poll `status` instead of taking the
 *                   notify ISR, mirroring Node A's Seam-1 approach.
 */

#endif /* NODE_B_IPC_MAILBOX_MAP_H */
