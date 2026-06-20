# ADR-0006: Security architecture — vetted primitives, two-layer secure boot

**Status:** accepted · **Date:** architecture phase

## Context
The project needs secure boot and authenticated bus communication. Rolling custom crypto is
an anti-pattern and a red flag in automotive. The value to demonstrate is *security
architecture*, not primitive implementation.

## Decision
- Use **vetted primitives**: the hardware crypto block (AES/SHA/ECC/TRNG) via Infineon's
  crypto driver, offloaded to the **Cortex-M0+**. `shared/crypto` is a thin wrapper exposing
  a clean service interface; it implements no algorithms. (mbedTLS / AUTOSAR Csm are
  documented alternatives.)
- **Secure boot in two layers:** (1) ROM verifies the FBL — root of trust, configured not
  written; (2) the FBL verifies the application image before jump — manifest, hash,
  signature — which we design and implement.
- **Bus security:** `shared/secoc` adds MAC + freshness to control messages, using the M0+
  crypto service.
- **Key handling** (storage, lifecycle, secret vs public) is an explicit design section.

## Consequences
- (+) Correct, mature posture; strong interview narrative ("I designed the chain, I didn't
  reinvent AES").
- (+) Crypto offload ties cleanly into the asymmetric dual-core story.
- (−) Depends on vendor crypto specifics that must be confirmed in the TRM/app notes.
- (−) Root-of-trust config (eFuse/lifecycle) is irreversible on real silicon — handle with
  care; document before touching fuses.

## Alternatives considered
- Hand-rolled crypto: rejected outright (insecure, red flag).
- App-level verification only, no ROM root of trust: rejected — verification without an
  anchored root of trust provides no real guarantee.
