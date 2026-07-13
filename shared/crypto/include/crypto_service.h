/*
 * crypto_service.h — the FBL-side client (ADR-0016 orchestration, ADR-0017).
 *
 * The one call the boot-decision path makes when FBL_DIGEST_ALGO == SHA256:
 * it builds a VERIFY_IMAGE request, hands it to the transport (ipc_mailbox),
 * awaits the verdict, and returns it. The call site in shared/boot does NOT
 * change (ADR-0016 D3 / ADR-0012 D6) — only what sits behind it grows this
 * cross-core hop.
 *
 * Fail-safe composition (ADR-0016 D5): ANY IPC failure — busy, timeout,
 * malformed reply — collapses to CRYPTO_VERDICT_ERROR, which the boot layer
 * treats identically to INVALID (stay in FBL, never jump). Never a hang, never
 * a silent "trust it".
 */
#ifndef CRYPTO_SERVICE_H
#define CRYPTO_SERVICE_H

#include "crypto_types.h"
#include "ipc_mailbox.h"

/* Timeout on the M0+ round trip (ADR-0017 D2 / ADR-0018 D4). */
#ifndef CRYPTO_VERIFY_TIMEOUT_MS
#define CRYPTO_VERIFY_TIMEOUT_MS   1000U
#endif

/* Bind the transport port (composition root). */
void crypto_service_init(const ipc_port_if_t *port);

/* Ask the M0+ to verify the app image at [base, base+len) against key_id
 * (hash + signature, two-stage on the M0+ side — ADR-0016 D2). Returns the
 * verdict; any transport failure ⇒ CRYPTO_VERDICT_ERROR (⇒ fail-safe). */
crypto_verdict_t crypto_verify_image(uint32_t base, uint32_t len, uint32_t key_id);

#endif /* CRYPTO_SERVICE_H */
