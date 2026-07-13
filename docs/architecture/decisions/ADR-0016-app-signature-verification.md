# ADR-0016: App signature verification — the FBL→app authenticity layer (M4)

**Status:** accepted · **Date:** 2026-07-05

## Context

After M3 the FBL is field-flashable over the bus (ADR-0012–0015), but it only checks
**CRC32** before jumping to a freshly-downloaded app — completeness, not authenticity
(ADR-0008 D3's honesty note). So any tester that gets past M3's deliberately-weak seed/key
can flash an **arbitrary** app image and the FBL will run it. M4 closes that: the FBL jumps
only to an image signed by the legitimate build key.

This ADR owns the **trust-chain** boundary — *what* is verified and in *what order*. The
*how* (crypto on the M0+) is ADR-0017; the *IPC* is ADR-0018; *keys* are ADR-0019.

**Scope boundary set with the project owner (this session):** M4 builds the **FBL→app**
authenticity layer in full. It does **not** touch device lifecycle or eFuse — the **ROM→FBL**
anchor (ADR-0006, ADR-0008 D5) stays designed-in-principle but **unprovisioned** on this dev
board. This is deliberate and it has an honesty consequence, stated in D4.

## Decisions

### D1. The image layout does not change — M4 populates fields ADR-0008 D3 already reserved

ADR-0008 D3 locked the layout specifically so this milestone would be additive:

```
[app_base .. app_base+image_len)   ── COVERED ──  vector table + descriptive header + code
app_base + image_len               ── EXCLUDED trailer ──
   M1/M3:  digest (CRC32, 4 B)
   M4:     hash[32] + signature[] + key_id (u32)
```

The covered range, the "digest never covers itself" property, and the fixed-offset header
parse are all unchanged. M4 fills `hash[32]`, `signature[]`, `key_id` — it does **not** widen
the covered range or move the trailer. Any pull to do so is a signal something upstream was
mis-scoped; stop and re-check D3 rather than change it.

### D2. Sign-over-hash, verified in two localizable stages

The signature is over the **32-byte hash**, not the raw image (the layout — `hash[32]` then
`signature[]` — already implies this). Verification is therefore two stages:

1. **integrity** — recompute SHA-256 over the covered body, compare to the stored `hash[32]`;
2. **authenticity** — verify `signature[]` against that hash with the `key_id`-selected public
   key (ADR-0019).

Storing the hash (rather than signing the raw image and recomputing only inside verify) costs
32 bytes and buys a **failure that localizes**: "image corrupt/incomplete" (stage 1 fails) is
a distinct, reportable outcome from "image unsigned or wrong key" (stage 2 fails) — useful to
the UDS `routineControl` check-image response and to a human reading a bring-up log. It also
means the stage-1 body-hash is the exact analogue of the M1/M3 completeness check, so the
"CRC32 → SHA-256" story is one covered-range, two algorithms, as D3 promised.

### D3. `FBL_DIGEST_ALGO` flips to SHA256; the two call sites do not change — ADR-0012 D6 verdict

ADR-0012 D6 bet that a compile-time macro (`FBL_DIGEST_ALGO`, ADR-0008 D3) resolves
"CRC32 now, real crypto later" for the single FBL image, so **no runtime `verify_if_t`
Strategy struct is needed** and the two verify call sites — the boot-decision tree
(ADR-0008 D1 step 4) and the UDS `routineControl` check-image handler (ADR-0012) — don't
change. **M4 is the test of that bet, and the verdict is: it holds, with one amendment.**

- **Holds:** the call sites are unchanged. Both still call `fbl_app_image_valid()`; the
  selection is still one compile-time axis. No function-pointer Strategy struct is introduced.
- **Amendment:** what sits *behind* the call now grows a **cross-core hop with new failure
  modes** (a busy or unresponsive M0+, a malformed response) that a local CRC32 never had.
  D6's implicit assumption — "verify can only fail by mismatch" — does **not** survive. So
  `fbl_app_image_valid()` gains a genuine *error* return distinct from *invalid*, and both map
  to the same fail-safe (D5). **D6's "no new interface type" stands; D6's "verify can't error"
  does not.** This is a real, citable finding for the M3 retrospective — the seam held, the
  failure surface behind it widened.

### D4. Honesty: mechanism real, board not trusted (the #5 scope consequence)

Because the ROM→FBL root is **not** provisioned (scope decision above), M4 makes the
verification *mechanism* real but does **not** make the board *secure*. An attacker with
physical/debug access can still rewrite the FBL itself, and then its "app signature check" is
whatever they replaced it with. Per ADR-0008 D5's standing rule — "until [lifecycle
provisioning], the system is not actually secure, and the design says so plainly" — this ADR
states it: **a green verify on this dev board proves the mechanism works, not that the device
is trustworthy.** Provisioning the root of trust is a separate, gated, irreversible action
that is explicitly out of M4.

### D5. Verify failure composes into the existing fail-safe — no new safe state

Both new failure kinds — *authenticity mismatch* (wrong/no signature) and *IPC error* (M0+
dead, timeout, malformed response) — route into **ADR-0008 D1 step 5 / ADR-0015 D7 row 1**:
invalid ⇒ stay in FBL, never jump. A dead M0+ must **never** hang the super-loop (a
bootloader hang is worse than a refused jump) and must **never** silently fall through to
"trust it." This is failure *composition*, not a new mechanism — the ADR-0015 D7 catalog gains
no row, it reuses row 1.

## Host/target split (ADR-0001)

| Host-testable core | Target-only (behind ports) |
|---|---|
| Trailer parse of the new `hash`/`signature`/`key_id` fields | reading app flash (memory-mapped) |
| Two-stage verify orchestration (D2), incl. the error path | the actual SHA-256 + signature (ADR-0017, on the M0+) |
| The verify-error → fail-safe mapping (D5), faked "M0+ errored" | — |

A host test fakes "the M0+ returned {valid / invalid / error}" and drives the whole FBL-side
decision without any M0+ code existing — same posture as ISO-TP in M3.

## Consequences

- (+) Additive: no image-layout change, no call-site change; the milestone is "populate the
  reserved trailer + swap the algorithm behind an unchanged call."
- (+) Two-stage verify gives a diagnosable failure instead of one opaque "invalid."
- (+) The D6 bet is settled with evidence, not asserted — good retrospective material.
- (−) The board is not actually secured (D4), by explicit scope choice; the mechanism is
  provable but the trust anchor is not set.
- (−) Verify now depends on a second core being alive (ADR-0017/0018) — a new liveness
  dependency the CRC32 path never had, mitigated by D5's timeout→fail-safe.

## Alternatives considered

- **Sign the raw image, no stored hash** — rejected (D2); saves 32 B but loses the
  integrity-vs-authenticity failure split and the CRC32→SHA-256 symmetry.
- **Introduce a runtime `verify_if_t` Strategy struct now** — rejected (D3); the call sites
  don't change and the selection is one compile-time axis, so a function-pointer indirection
  would be a second selection mechanism for a concern already resolved — exactly what
  ADR-0012 D6 rejected, and M4 confirms that judgment.
- **M4-local verify (SHA-256 + ECDSA on the CM4, public key in FBL flash), defer the M0+
  service to M5** — a legitimate alternative that is *equally secure against M4's actual
  threat* (verify needs only the public key). Rejected because it guts the milestone's
  headline dual-core-offload story and only defers the identical M0+ work to M5, where the
  secret MAC key makes offload non-optional anyway. See ADR-0017 Context for the full
  "does offload even buy security here" argument that this rejection rests on.

## Review history

Design-reviewed in discussion with the project owner (this session): threat model first,
then the two boundaries (trust chain here; crypto service in ADR-0017). Decisions #1–#4 from
that session map to: keep the M0+ offload (ADR-0017), coarse RPC (ADR-0017 D1), key in the
M0+ image (ADR-0019), ECDSA P-256/SHA-256 (ADR-0019); #5 = ignore lifecycle/eFuse, recorded
as D4 here. Module skeletons + failing Unity tests next, then seam-by-seam bring-up; silicon
findings get appended here as in ADR-0008/0012/0013.
