# ADR-0017: M4↔M0+ crypto-service architecture — the offload boundary (M4)

**Status:** accepted · **Date:** 2026-07-05

## Context

ADR-0016 needs SHA-256 + signature verification. The roadmap and ADR-0006 commit to running
crypto on the **Cortex-M0+**, not the M4. Before adopting that by inertia, the design session
red-teamed it, because an architect who can't say *why* they offload is cargo-culting an HSM:

> **Signature verification needs only the *public* key, and a public key is not secret.** So
> the classic HSM argument — "keep the secret key where compromised code can't read it" — does
> **not** apply to M4's threat (a remote attacker flashing an unsigned app over the bus; see
> ADR-0016 Context). Verification could run on the M4 with the public key in FBL flash and be
> *exactly as secure against that threat*. And offload does **not** protect the *decision*: the
> M4 still performs the jump, so a hypothetically runtime-compromised M4 could ignore a "no."
> Offload protects the key and the hash computation, never the branch instruction.

So we adopt the offload **consciously, for three legitimate reasons that are not "verify needs
a vault":**

1. **Forward design to M5 (the real reason).** SecOC needs a **secret MAC key used at runtime
   by the attacker-reachable app**. *That* genuinely must be offloaded. M4 builds the service;
   signature-verify is its first, easy customer; M5's MAC op plugs into the same dispatch.
2. **TCB isolation.** The crypto engine + key storage stay unreachable from the FBL's large
   attack surface (the CAN/UDS parser), so a memory bug there can't reach crypto state or key
   material — protects key *integrity* even where confidentiality isn't the point.
3. **Root-of-trust cleanliness.** The M0+ is the ROM-started, first-verified core (overview §6:
   it boots first and starts the CM4). Anchoring crypto to it puts trust where the root is.

**Honesty carried into the write-up (ADR-0016 D4):** for M4 *in isolation* the offload is
forward-design + isolation, not security-critical for verify. We say so rather than overclaim.

**Scope reality this creates:** today the M0+ runs the **vendor prebuilt** blob that only
starts the CM4 (overview §6). Offload means **we now own a custom M0+ image** that both starts
the CM4 *and* runs a crypto-service loop — a genuine scope increase (D4), named up front.

## Decisions

### D1. Coarse RPC — the M0+ hashes the real flash itself ("verify this image")

The FBL sends `{op = VERIFY_IMAGE, base, len, key_id}`. The M0+ **reads the app-flash region
itself, computes SHA-256 over it, checks the signature, returns a verdict.** The FBL does not
compute the hash and does not see the primitives.

Rationale is a security-layering argument, not just "smaller IPC": if the M4 computed the
hash and the M0+ only verified sig-over-a-supplied-hash, the M0+ would be trusting a hash the
*M4 reported*, never seeing what is actually in flash. **The trusted core must verify reality,
not a report about reality** — it hashes the bytes that are really there. For M4's threat the
M4 isn't lying at boot, so a fine RPC would be *acceptable*; coarse is *stronger by
construction* and is the one we build.

**Dependency to confirm** *(verify in TRM)*: the M0+ must be able to read-map the code-flash
region where the app lives. Code flash is a shared address space (overview §6) so this is
expected, but D1 rests on it. **Fallback if false:** a *streaming* variant — the M4 feeds
flash blocks to the M0+ to hash — which preserves D1's semantics (M0+ hashes real bytes) with
different plumbing; the request op stays `VERIFY_IMAGE`, only the data path changes.

### D2. Synchronous, blocking RPC with a hard timeout → fail-safe

At boot the FBL super-loop has nothing to do but wait for the verdict (the same reasoning that
made M3's transport poll-based, ADR-0012 D2). So the RPC is **synchronous and blocking** — no
async machinery to invent. The **non-negotiable**: the wait has a bounded timeout, and the
timeout lands in the existing fail-safe (ADR-0016 D5 / ADR-0008 D1 step 5): a dead or busy M0+
⇒ "app invalid, stay in FBL," **never a hang, never a silent trust.**

**Alternative:** async request + poll each super-loop iteration (M3's `isotp_poll` rhythm).
Buys nothing at boot where the FBL has no other work; the only future scenario that needs it
is the crypto service running *concurrently with* an in-progress UDS download — not an M4
requirement. Noted, not built.

### D3. Four layers, strict one-directional dependency (mirrors ADR-0012, across a core)

```
IPC transport  ←  service protocol (envelope + dispatch)  ←  op handlers  ←  back end
(ADR-0018)         (op code, length, opaque payload)          hash / verify    HW crypto
                                                              [M5: mac]         driver + keys
```

- **Transport** (ADR-0018) moves an opaque request/response buffer across the core boundary;
  knows nothing about which op it carries — same discipline as ISO-TP handing up a buffer.
- **Service protocol** owns a minimal envelope `{op_code, length, payload}` and the M0+-side
  dispatch loop. Knows the *shape* of a crypto request; knows nothing about the FBL's
  boot-decision logic above or the mailbox wire format below.
- **Op handlers** — one per primitive behind a common interface: `HASH` (SHA-256),
  `VERIFY_IMAGE` (hash + signature over a flash range, D1), and a **reserved** slot for M5's
  `MAC`/`HMAC`. Adding an op is a new dispatch-table row, not a protocol change.
- **Back end** — the HW crypto block driver calls and key-storage read (ADR-0019). No RPC
  semantics here (mirrors M3's "operations layer knows nothing about UDS").

The M0+ side is thus **its own tiny layered stack**: notify-handler → envelope decode/dispatch
→ op handler → HW driver. Its host-testable half is the envelope/dispatch logic (fake the
mailbox); its target-only half is the notify wiring, the HW crypto driver, and the key read.

### D4. We own the M0+ image; it keeps starting the CM4 and adds the service loop

The custom M0+ image must **(1)** preserve the existing "start the CM4" responsibility
(overview §6 — nothing else boots the CM4) and **(2)** run the dispatch loop of D3. This is a
new ModusToolbox CM0+ project replacing the vendor prebuilt *(exact MTB config + how the CM4
start sequence is preserved: verify against the BSP/startup on bring-up)*. It is the milestone's
biggest single lift and is called out here so it is planned, not discovered.

### D5. The IPC boundary is on-chip — not a trust boundary, so no anti-replay

The mailbox is on-die, M4↔M0+. Unlike SecOC on the CAN bus (M5), it is **not** exposed to a
remote attacker, so the RPC needs **no freshness/anti-replay/MAC on the envelope itself**.
Naming when you *don't* need replay protection is as much the point as M5's case where you do —
the same word "RPC," a completely different threat surface.

## Host/target split (ADR-0001)

| Host-testable core | Target-only (behind ports) |
|---|---|
| Envelope framing `{op_code, length, payload}` | the mailbox + HW semaphore + notify (ADR-0018) |
| Dispatch table (op → handler), incl. the M5-reserved slot | the M0+ image itself (its own build, D4) |
| The M4-side request/await/verdict flow, incl. timeout→fail-safe | HW crypto block driver (SHA/ECC) |
| — | key-storage read (ADR-0019) |

A host test fakes "the M0+ responded with {valid/invalid/error}" and drives the full FBL-side
path with no M0+ code present.

## Consequences

- (+) The offload is adopted with an explicit, honest rationale (M5 seam + isolation + root
  cleanliness), not overclaimed as verify-security.
- (+) Coarse RPC makes the trusted core verify real flash contents — correct by construction.
- (+) One dispatch table serves M4 (verify) and M5 (MAC) — the additive-op property is the
  payoff of designing the service once.
- (−) We now own and must maintain a custom M0+ image (D4) — the real cost of the offload.
- (−) Boot-time verify now depends on M0+ liveness — mitigated by D2's timeout→fail-safe.
- (−) D1 depends on the M0+ reading app flash (TRM-pending); the streaming fallback covers
  the "false" case at the cost of more IPC traffic.

## Alternatives considered

- **M4-local verify, no M0+ service (defer to M5)** — equally secure for M4's threat; rejected
  (Context) because it defers identical work to where offload is non-optional and drops the
  milestone's dual-core story.
- **Fine RPC (M4 hashes, M0+ verifies sig-over-hash)** — smaller M0+ surface; rejected (D1),
  the M0+ must hash real flash, not a reported hash.
- **Async/polled RPC** — rejected (D2); no concurrent work at boot to justify it.
- **Keep the vendor M0+ prebuilt and run crypto on the M4** — that is just "M4-local verify"
  by another name; rejected as above.

## To verify in the TRM / on silicon

- Whether the M0+ can read-map the app code-flash region (D1; else streaming fallback).
- How to replace the vendor CM0+ prebuilt while preserving the CM4 start sequence (D4).
- HW crypto block capabilities driving ADR-0019's algorithm pick (ECDSA vs Ed25519).

## Review history

Design-reviewed with the project owner (this session); decision #1 (keep offload) and #2
(coarse RPC) accepted on the recommendations above. Skeletons + failing Unity tests next
(envelope, dispatch, request/await/timeout), then seam-by-seam bring-up — including the D4
M0+-image stand-up, expected to be the finding-rich seam. Silicon findings appended here.
