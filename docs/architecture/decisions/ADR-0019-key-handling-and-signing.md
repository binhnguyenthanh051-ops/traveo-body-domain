# ADR-0019: Key handling, algorithm, and the signing tool (M4)

**Status:** accepted · **Date:** 2026-07-05

## Context

ADR-0016 verifies a signature; ADR-0017 runs it on the M0+. This ADR owns the **keys** — what
is secret vs public, where the public material lives, what `key_id` selects, which algorithm,
and how images get signed. `CLAUDE.md`/ADR-0006 forbid hand-rolled crypto: `shared/crypto`
wraps vetted primitives and implements no algorithms. Key *handling* — as opposed to key
*algorithms* — is an explicit design section (ADR-0006) and a strong interview surface.

## Decisions

### D1. Public/private split — the private key never touches the device

- **Private signing key** lives only on the **build host**, owned by a new host-side signing
  tool (D5). It never ships in any image, never lands in flash, never crosses the bus.
- **The device holds only public material** (the verification key, or its hash). Verification
  needs nothing secret (ADR-0017 Context) — so nothing on the device can *produce* a valid
  signature. If any design has the device able to sign, that is the smell to kill on sight.

This is the whole reason M4's "security on the device" is defensible without an HSM-grade
secret store: **there is no on-device secret to steal for signature verification.** (M5's MAC
key *is* secret and *is* why the M0+ offload exists — ADR-0017 Context reason 1.)

### D2. The public key is embedded in the M0+ image

The verification public key is **compiled into the custom M0+ image** (ADR-0017 D4), not stored
in a separate flash region.

- **Why:** it anchors key *integrity* to the ROM-verified, first-booting core for free — an
  attacker who somehow reaches FBL flash still can't swap the key, because the key isn't there.
  "Rotate the key = rebuild + reflash the M0+ image" is a clean, defensible lifecycle that
  rides the existing root of trust rather than inventing a second protected store.
- **Alternative:** a dedicated key-storage flash region, rotatable without rebuilding the M0+.
  Rejected for M4 — that region would then need its *own* integrity protection (else it's the
  weak link), which is machinery a single-key milestone doesn't warrant. Deferred to whenever
  multi-key/rotation is a real requirement; D3's `key_id` leaves the door open.

### D3. `key_id` is a table lookup with one row today, designed for more

The trailer's `key_id` (u32, reserved by ADR-0008 D3) selects *which* public key verified an
image. M4 implements the selection as a **table lookup that happens to have one entry**, so a
later milestone adds rows (key rotation, per-supplier keys, a dev vs production key), not
plumbing. An unknown `key_id` is a verify **failure** (⇒ fail-safe, ADR-0016 D5), never a
"default to key 0" — an attacker must not be able to select a weaker/absent key by naming it.

### D4. ECDSA P-256 over SHA-256

- **Hash:** SHA-256 (already fixed by ADR-0008 D3's `hash[32]` / `FBL_DIGEST_ALGO = SHA256`).
- **Signature:** **ECDSA on P-256.** It is the automotive-conventional pick, the HW crypto
  block accelerates ECC, and keys/signatures are small — a P-256 signature (~64–72 B) sits
  comfortably in the excluded trailer.
- **Alternatives:** **RSA-2048** — simpler verify math but bulky keys/signatures, wasteful in
  the trailer; rejected. **Ed25519** — attractive (deterministic, fast, no per-signature
  nonce risk) but HW-block support on this part is uncertain *(verify in TRM)*; if the block
  does Ed25519 cleanly it is a reasonable swap, but ECDSA P-256 is the default we design to.
- The choice is isolated behind ADR-0017's `VERIFY_IMAGE` op and `shared/crypto`, so a
  TRM-driven swap to Ed25519 is a back-end change, not a layout or protocol change.

### D5. A host-side signing tool owns the private key and the trailer

A new `host_tools/` tool (Python, per project convention) takes a built app image and:

1. computes SHA-256 over the covered range (ADR-0008 D3 / ADR-0016 D2);
2. signs the hash with the private key (D1);
3. writes the `hash[32] + signature[] + key_id` trailer at `app_base + image_len`.

Its output is a signed image the **M3 UDS flash tool** (`host_tools/uds_flash`) then downloads
unchanged — signing is a *build-time* step, flashing is the *existing* transport, and the two
stay separate tools. The private key is managed as a host-side secret (file/keystore, out of
firmware scope); key *provisioning onto the device* is moot here because D2 compiles the
public key into the M0+ image at *its* build time.

## Host/target split (ADR-0001)

| Host-testable core | Target-only |
|---|---|
| `key_id` → public-key selection table (D3), incl. unknown-id → fail | the embedded public key bytes in the M0+ image (D2) |
| Trailer field layout the signing tool and FBL must agree on (D5) | HW crypto block ECDSA verify (D4, ADR-0017 back end) |

The signing tool (D5) gets its own `pytest` coverage (host_tools convention); a round-trip
test — sign a fixture image, verify the trailer parses and the `key_id` selects the fixture
key — ties the tool and the FBL parse together without the target.

## Consequences

- (+) No on-device secret for verification (D1) — the security story needs no HSM-grade vault
  for M4, and that is stated honestly rather than dressed up.
- (+) Key integrity rides the existing root of trust (D2) — no second protected store to
  build or defend.
- (+) `key_id` (D3) makes rotation/multi-key additive later; ECDSA behind an op (D4) makes an
  algorithm swap a back-end change.
- (−) Rotating the single key means rebuilding + reflashing the M0+ image (D2) — acceptable
  for one key, revisited if rotation becomes routine.
- (−) The signing tool now holds a private key (D5) — real key-management hygiene the project
  must not fake; treated as a host-side secret, and a full key-management lifecycle
  (revocation, expiry, HSM-backed signing) is design-essay scope (roadmap EP.17), not built.

## Alternatives considered

- **Public key in a dedicated flash region** — rejected (D2); needs its own integrity guard.
- **RSA-2048 / Ed25519** — RSA rejected (bulk), Ed25519 TRM-pending (D4).
- **`key_id` defaults to key 0 when unknown** — rejected (D3); lets an attacker name away the
  real key.
- **Provision the public key via a device programming step instead of compiling it in** —
  rejected for M4; that reintroduces a provisioning/lifecycle action (#5, out of scope) for no
  benefit while there is exactly one key.

## To verify in the TRM / on silicon

- HW crypto block ECDSA-P256 (and whether Ed25519 is cleanly supported) — D4.
- The mechanics of embedding + reading the public key in the CM0+ image (D2) alongside the
  ADR-0017 D4 M0+-image stand-up.

## Review history

Design-reviewed with the project owner (this session); decisions #3 (key in the M0+ image) and
#4 (ECDSA P-256/SHA-256) accepted on the recommendations above. The signing tool + its pytest
round-trip land with the ADR-0016/0017 skeletons; the D4 algorithm is confirmed against the
crypto block during ADR-0017 D4 bring-up, findings appended here.
