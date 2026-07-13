/*
 * fbl_crypto.h — CM4-side crypto/IPC target binding (M4 Seam 1+).
 *
 * Exposes the hardware-bound ipc_port_if_t (ADR-0018) that the host-tested
 * transport (shared/crypto/ipc_mailbox.c) and, later, the verify client
 * (crypto_service.c) run on. Target-only; not in host CI.
 */
#ifndef FBL_CRYPTO_H
#define FBL_CRYPTO_H

#include "ipc_mailbox.h"   /* ipc_port_if_t */
#include "crypto_types.h"  /* crypto_verdict_t */
#include <stdbool.h>
#include <stdint.h>

/* Initialise the mailbox to IDLE and return the port bound to Cy_IPC_Drv on
 * IPC_CRYPTO_CHANNEL. Call once before the first transaction. */
void                  fbl_crypto_port_init(void);
const ipc_port_if_t  *fbl_crypto_port(void);

/* Seam-2 bring-up: ask the CM0+ to SHA-256 the NIST "abc" vector over the real
 * mailbox + HW crypto path, and check the digest matches the known answer.
 * Returns true on an exact match. (The Seam-1 fbl_crypto_bringup_echo() was
 * retired — see port_crypto.c — since the CM0+ now runs crypto_dispatch; this
 * exercises the same transport through a real request.) */
bool                  fbl_crypto_bringup_hash(void);

/* Seam-3 bring-up: ask the CM0+ to VERIFY_IMAGE a signed image at [base,
 * base+len) against key_id (the real crypto_verify_image client). Drive it at
 * the bench with a sign_image.py-signed blob flashed to a SCRATCH address —
 * expect VALID for a good image, INVALID for a tampered one or wrong key.
 * Returns the raw verdict so the bench can tell VALID / INVALID / ERROR apart. */
crypto_verdict_t      fbl_crypto_bringup_verify(uint32_t base, uint32_t len,
                                                uint32_t key_id);

#endif /* FBL_CRYPTO_H */
