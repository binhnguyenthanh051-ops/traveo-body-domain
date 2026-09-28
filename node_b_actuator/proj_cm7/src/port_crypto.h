/*
 * port_crypto.h — Node B CM7 crypto-offload seam (ADR-0018 / ADR-0021).
 *
 * Sets up the MPU non-cacheable mailbox region (D6), initialises the shared
 * mailbox, and binds the ipc_port_if_t transport into crypto_service so the app
 * can call crypto_mac(). The SecOC oracle (secoc_mac_if_t) then binds directly
 * to crypto_mac in the SecOC-wiring seam.
 */
#ifndef PORT_CRYPTO_H
#define PORT_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>
#include "ipc_mailbox.h"   /* ipc_port_if_t */

/* MPU non-cacheable setup + mailbox idle + crypto_service_init(port). Call once
 * at app start, before the first crypto_mac(). */
void secoc_crypto_port_init(void);

/* The bound transport port (for direct ipc_transact use / tests). */
const ipc_port_if_t *secoc_crypto_port(void);

#if defined(SECOC_CRYPTO_BRINGUP) && (SECOC_CRYPTO_BRINGUP != 0)
/* On-silicon offload round-trip self-check (task context). 0 = pass, else the
 * failed check. See port_crypto.c. */
uint32_t secoc_crypto_bringup_mac(void);

/* Bench Stage 3.3 + 3.4: run the round trip and log it + the cache/MPU state as
 * LOG_EVT_DBG_U32 records (task context). */
void secoc_crypto_bringup_report(void);
#endif

#endif /* PORT_CRYPTO_H */
