# shared/crypto — crypto service + cross-core IPC (M4)

Thin wrapper over vetted primitives (HW crypto block on the M0+). Implements
**no algorithms** (ADR-0006) — SHA-256/ECDSA live behind the M0+ back end,
target-only. This module is the **host-testable** layers of the M4 crypto
service: framing, transport protocol, dispatch, key selection, and the FBL-side
verify client.

Design: **ADR-0016** (app signature verification / the FBL→app trust layer),
**ADR-0017** (the M4↔M0+ crypto-service layering + the offload rationale),
**ADR-0018** (the shared-RAM mailbox + hardware-semaphore transport),
**ADR-0019** (key handling, ECDSA P-256/SHA-256, the signing tool).

## Layers (strict one-directional dependency, mirrors the M3 diag stack)

```
ipc_mailbox   ←  crypto_msg     ←  crypto_dispatch   ←  (M0+ back end: HW crypto)
(transport)      (framing)         (server dispatch)     target-only, faked in tests
                       ↑
                 crypto_service  ←  crypto_keystore
                 (FBL client)       (key_id → pubkey)
```

| File | Layer / role | ADR |
|---|---|---|
| `crypto_types.h` | op codes, verdict (INVALID=0 = safe default), the envelope | 0017 D3 / 0016 D2 |
| `crypto_msg.[ch]` | envelope encode/decode; malformed reply ⇒ rejected | 0017 D3 |
| `ipc_mailbox.[ch]` | acquire→write→notify→bounded-wait→read→release; reports timeout, caller decides policy | 0018 |
| `crypto_dispatch.[ch]` | M0+-side op→handler table; always answers (unknown op / malformed ⇒ ERROR verdict) | 0017 D3 |
| `crypto_keystore.[ch]` | `key_id`→pubkey; unknown id ⇒ NULL, never key 0 | 0019 D3 |
| `crypto_service.[ch]` | FBL client `crypto_verify_image()`; any IPC failure ⇒ ERROR ⇒ fail-safe | 0016 D5 / 0017 |

## Host/target split (ADR-0001)

Everything above is host-tested with fakes (`tests/ipc_port_fake.c` scripts the
"other side" of the mailbox — a canned reply, a denied semaphore, or silence).
Target-only, behind the ports: the real HW semaphore + IPC channel + notify
(ADR-0018 D1), the M0+ image and its HW crypto driver calls (ADR-0017 D4), and
the public key bytes compiled into that image (ADR-0019 D2).

The one FBL-side call site does **not** change (ADR-0016 D3 / ADR-0012 D6): the
boot decision still calls `fbl_app_image_valid()`; only what sits behind it,
when `FBL_DIGEST_ALGO == SHA256`, grows the cross-core hop.

## Status

**M4 host-testable core implemented** — `make test_crypto_*` green (22 Unity
tests across the five suites), cppcheck clean. The signing tool
`host_tools/sign_image.py` (+ `host_tools/tests/test_sign_image.py`) is
implemented (ECDSA P-256 over SHA-256, raw `r||s` trailer).

Still target-only / not yet built: the real HW semaphore + IPC channel + notify
wiring, the custom M0+ image and its HW crypto driver (ADR-0017 D4), the public
key bytes in that image (ADR-0019 D2), and the `FBL_DIGEST_ALGO == SHA256`
composition wiring `fbl_app_image_valid()` to `crypto_verify_image()`. Those
land in on-board bring-up.
