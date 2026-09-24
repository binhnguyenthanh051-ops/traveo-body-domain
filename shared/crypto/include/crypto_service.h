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

/* MAC round-trip timeout (ADR-0021 D7). Much tighter than the boot verify: this
 * is the runtime control path, and the op is a CMAC over ~a dozen bytes, so the
 * cost is the IPC hop, not the crypto. Bench-tunable against the control budget
 * (secoc-architecture §8.4). */
#ifndef CRYPTO_MAC_TIMEOUT_MS
#define CRYPTO_MAC_TIMEOUT_MS      10U
#endif

/* Bind the transport port (composition root). */
void crypto_service_init(const ipc_port_if_t *port);

/* Ask the M0+ to verify the app image at [base, base+len) against key_id
 * (hash + signature, two-stage on the M0+ side — ADR-0016 D2). Returns the
 * verdict; any transport failure ⇒ CRYPTO_VERDICT_ERROR (⇒ fail-safe). */
crypto_verdict_t crypto_verify_image(uint32_t base, uint32_t len, uint32_t key_id);

/* Ask the M0+ to AES-CMAC `msg` under key_id, writing the full 16-byte tag on
 * success (ADR-0021 D7). Returns false on ANY failure — no port, unencodable
 * request, transport busy/timeout/malformed, or an M0+ error reply (unknown
 * key_id ⇒ a verdict, not a tag) — so the SecOC caller drops the frame
 * (fail-safe, REQ-SECOC-001/010). The signature matches secoc_mac_if_t.mac, so
 * the app binds this directly as the SecOC oracle. */
bool crypto_mac(uint32_t key_id, const uint8_t *msg, size_t msg_len,
                uint8_t tag[CRYPTO_CMAC_TAG_LEN]);

#endif /* CRYPTO_SERVICE_H */
