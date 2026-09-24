/*
 * port_crypto.h — Node A APP crypto-offload seam (ADR-0017/0018, M5).
 *
 * The app is the second client of the M0+ crypto service (the FBL was the
 * first — arch §3). It reuses the FBL-proven mailbox transport (same Node A map,
 * same IPC channel; FBL and app never run at once) with an app-side clock. No
 * MPU non-cacheable region here — the M4 has no cache (unlike Node B's CM7).
 */
#ifndef PORT_CRYPTO_H
#define PORT_CRYPTO_H

#include "ipc_mailbox.h"   /* ipc_port_if_t */

/* Idle the mailbox and bind the transport into crypto_service. Call once at app
 * start, before the first crypto_mac(). */
void secoc_crypto_port_init(void);

/* The bound transport port. */
const ipc_port_if_t *secoc_crypto_port(void);

#endif /* PORT_CRYPTO_H */
