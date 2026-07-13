# M4 — App secure boot + crypto offload (architecture-first brief)

> **Role:** act as an **automotive diagnostic-stack architect** (crypto/security lens this
> time), not a coder. The FIRST and ONLY deliverable of the opening session is a **layered
> architecture design** — no service code, no signing sequence yet. Tradeoffs + at least one
> alternative at every layer boundary (the reasoning is the blog/interview material and the
> point of the milestone). Same rhythm afterwards: discuss → agree → ADR(s) → failing host
> tests → implement → on-board bring-up.

## Where this sits

M1 (FBL boots→verifies→jumps, CRC32 placeholder), M2 (app on FreeRTOS, CAN live), and M3
(UDS reprogramming — the FBL is field-flashable over the bus, ADR-0012–0015) are **done and
tagged**. M4 makes the trust chain real: the FBL verifies the app's **authenticity**, not
just its completeness, and that verification is backed by vetted crypto primitives running
on the **Cortex-M0+**, not hand-rolled and not run on the same core that decides to trust
the result.

Two things were deliberately **left as seams, not features**, for exactly this moment:

- **ADR-0008 D3** already laid out the image trailer as `hash[32] + signature[] + key_id`
  (excluded from the digest range) and made the digest algorithm a compile-time macro
  (`FBL_DIGEST_ALGO`, `CRC32` today). M4 is the macro flip to `SHA256` plus the signature
  check — the layout does not change.
- **ADR-0012 D6** deliberately did *not* introduce a runtime `verify_if_t` Strategy struct,
  reasoning that D3's compile-time selection already resolves "CRC32 now, real crypto later"
  for the *single* FBL image. M4 is where that bet gets cashed: if flipping one macro plus
  adding a signature check turns out to be enough, D6 was right; if it isn't, that's a real
  finding for the M3 retrospective.

**Primary goal is architectural, not featural** (same discipline as M3). M4 is one piece of
the single layered diagnostic-and-security stack designed at M3 — it must slot in as a new
back-end behind the existing verify call site and a new service behind a stable RPC
interface, not a rewrite of the UDS server or the boot-decision tree.

## The architecture to design FIRST (the whole point of the opening session)

Unlike M3 (one stack, one node), M4 has **two boundaries to design**, and the session should
treat them as separate layered questions that meet at one interface:

### Boundary 1 — the trust chain (what gets verified, by what, in what order)

```
ROM (root of trust)  →  FBL verifies app  →  app runs
```

- **ROM verifies FBL**: already the design (ADR-0006, ADR-0008 D5) — an immutable root
  anchored in eFuse/secure flash. **Configuring** this (lifecycle advancement, key-hash
  provisioning) is a separate, gated, irreversible step — do not conflate "design the chain"
  with "burn the fuses." The session designs for it; provisioning is its own reviewed action,
  explicitly not bundled into this brief's deliverables.
- **FBL verifies app**: this is the part M4 actually builds — manifest/header (already
  defined, ADR-0008 D3), hash, signature, key handling. This is where the session's design
  effort goes.

### Boundary 2 — the crypto service (who computes the verify, and how the FBL asks)

This is the new layered stack, directly analogous to M3's transport→session→handler→
operations shape, but for a crypto RPC instead of a UDS request:

1. **IPC transport (M4↔M0+).** The physical carrier: shared-RAM mailbox + hardware
   semaphore for mutual exclusion (per `CLAUDE.md` — IPC uses hardware semaphores, not a
   software lock). Segments/frames nothing; it moves an opaque request/response buffer
   across the core boundary and knows nothing about what operation it carries. Hands the
   layer above a **complete buffer on both sides**, the same discipline as ISO-TP in M3.
2. **Crypto service protocol (dispatch).** A minimal request/response envelope (op code,
   length, opaque payload) and the M0+-side dispatch loop. Knows the *shape* of a crypto
   request, not the FBL's boot-decision logic and not the mailbox's wire format below it.
3. **Operation handlers.** One per primitive behind a common interface — hash (SHA-256),
   signature verify (algorithm TBD in-session), and a forward-looking placeholder for M5's
   MAC/HMAC (SecOC reuses this same service, per `overview.md` §5 — design the dispatch table
   so adding an op is additive).
4. **Back end.** The actual HW crypto block driver calls and key storage access on the M0+
   side. No RPC semantics live here, mirroring M3's "operations layer knows nothing about
   UDS."

### The seam that must exist from day one

- **The M4-side call site stays the existing one.** `fbl_digest()` /
  `fbl_app_image_valid()` (ADR-0008 D3, reused unchanged per ADR-0012 D6) is still the *only*
  place the boot-decision tree and the UDS `routineControl` check-image handler call into
  verification. M4's job is to make **what's behind that call**, when `FBL_DIGEST_ALGO ==
  SHA256`, issue an IPC request to the M0+ crypto service instead of computing CRC32 locally
  — the call site does not change, only its target-side implementation grows a cross-core
  hop. This is the direct continuation of D6's bet: confirm in-session whether it still
  holds, or whether the cross-core RPC is different enough (latency, failure modes, a second
  core that can be unresponsive) to need its own seam after all.
- **Key handling is a first-class design section, not an implementation detail.** Public
  verification key vs private signing key is a secret/non-secret split: the private key
  never touches the device (a host-side build/signing tool owns it — likely a `host_tools/`
  addition, out of firmware scope); the device only ever holds/derives the public key or a
  key hash. Where that public material lives (FBL flash region vs M0+-only storage vs
  fused/protected memory) and what `key_id` in the trailer actually selects (today: one key,
  designed for more) is a session-1 decision, not an afterthought.

### Patterns to name explicitly (defensible in an interview)

- **Layered architecture** with enforced dependency direction, mirroring M3's shape but
  across a core boundary instead of a bus.
- **Service/RPC boundary** as the security isolation mechanism — the M0+ is trusted
  precisely because the M4 (which runs the reprogrammable, attacker-reachable-over-CAN FBL
  and app) never holds the private key or the raw crypto primitive, only calls a service.
  This is the concrete architect answer to "why offload to a second core at all" — say it
  out loud in-session, don't let it be implicit.
- **Fail-safe composition** — an IPC failure (M0+ unresponsive, malformed response) must
  degrade to the *same* fail-safe as an invalid digest (ADR-0008 D1 step 5: stay in FBL,
  never jump), not a hang and not a silent "trust it anyway." Name this as a deliberate
  reuse of the existing fail-safe, not a new failure mode to invent.

## Host/target split (ADR-0001 — same forcing function as M3)

| Host-testable core | Target-only (behind ports) |
|---|---|
| Request/response envelope framing (op code, length, payload) | Hardware semaphore acquire/release, shared-RAM mailbox |
| Crypto-service dispatch table (op → handler) | The M0+ image itself (its own build, its own firmware) |
| Manifest/trailer parsing (`key_id`, hash, signature fields) | HW crypto block driver calls (SHA/ECC engine) |
| Key-selection logic (`key_id` → which public key) | Public key storage read (flash / protected region) |
| The IPC failure → fail-safe mapping | The actual cross-core fault/timeout behaviour |

If the layering is right, a host test can fake "the M0+ responded with X" and drive the
whole FBL-side path without any M0+ code existing yet — same test posture as ISO-TP in M3.

## Scope for M4 (keep it minimal, but architected)

In scope: the two-boundary layered design above; `FBL_DIGEST_ALGO = SHA256` wired through
the existing D3 call sites; a signature-verify operation added to the trailer's existing
`signature[] + key_id` fields; the M4↔M0+ crypto-service RPC (transport, dispatch, handler,
back-end layers); key handling design (public/private split, storage location, `key_id`
scheme); a minimal M0+ firmware image that only runs this crypto service (no other M0+
responsibilities yet); host tests with a faked M0+ response; on-board bring-up of the whole
cross-core verify path.

**Deliberately deferred** (design the seam, don't build):
- **Lifecycle advancement / fuse provisioning** (burning the ROM root of trust for real) —
  a separate, explicitly gated, irreversible action; design for it, don't execute it in this
  milestone.
- **SecOC / authenticated bus messaging** — **M5**; the crypto-service dispatch table is
  designed so an HMAC/MAC op is additive, but the op itself is not built now.
- **Key rotation / multi-key policy** — `key_id` is designed to allow it later; only one key
  is provisioned for M4.
- **App-side (data/DTC) diagnostics reuse of the UDS server** — unaffected by M4, no change
  expected.
- **A real key-management/HSM lifecycle (revocation, expiry)** — design essay only (this
  ties into the EP.17 architecture essay in the roadmap), not implemented.

## Deliverables (in order)

1. **The layered architecture design** — both boundaries (trust chain; crypto-service RPC),
   the interface at each layer, the key-handling design, tradeoffs + one alternative per
   boundary. **Stop here; wait for agreement.**
2. On agreement: **ADR(s)** — app-signature verification (extends ADR-0008 D3, confirms or
   revisits ADR-0012 D6's bet); the M4↔M0+ crypto-service layering; key handling and
   storage; the IPC transport (hardware semaphore + shared-RAM mailbox) design.
3. Module skeleton + interfaces (headers) for each layer, kept host-fakeable, including the
   M0+-side dispatch loop skeleton.
4. **Failing Unity tests** for the host-testable core — envelope framing, dispatch-table
   selection, manifest/trailer parsing with the new fields, key-selection logic, and the
   IPC-failure → fail-safe mapping (a faked "M0+ timed out" / "M0+ returned malformed
   response" case must never result in a jump).
5. Implement against tests; `make test` + `make lint` green. Target-only (hardware
   semaphore, mailbox, M0+ image, HW crypto driver calls, key storage read) behind ports.
6. On-board bring-up: sign a real app image with a host-side tool → flash it via the M3 UDS
   path → FBL boots → issues the cross-core verify request → M0+ computes hash + checks
   signature → FBL gets a trust/no-trust answer → jumps only on trust. Then the failure demo:
   flash a validly-CRC-complete but wrongly-signed image (or stall the M0+) and show the FBL
   refuses to jump.

Start with step 1 only. Wait for agreement before ADRs or code.

## Watch-fors (call these out, don't gloss)

- **Conflating "design the root of trust" with "burn the fuses."** The chain gets designed
  now; lifecycle/fuse provisioning is a separate, later, explicitly-approved action
  (ADR-0008 D5 already said this for M1 — it still holds at M4).
- **Private key touching the device, ever.** Only public material (or its hash) lives on
  the M0+/FBL side. If the design has the device holding anything that could sign a new
  image, that is the smell to catch in-session, not after.
- **IPC failure must fail safe, not hang.** A busy/dead M0+ must time out into "app invalid,
  stay in FBL" — never a super-loop stall (this is a bootloader; a hang here is worse than a
  refusal to jump) and never a silent fall-through to "trust it."
- **Shared-memory race without semaphore discipline.** The mailbox is shared RAM; skipping
  the hardware semaphore (or using it inconsistently on the two sides) is exactly the kind
  of bug that works in bring-up and fails intermittently later — design the acquire/release
  protocol explicitly, don't leave it implicit.
- **Re-litigating ADR-0012 D6 honestly.** D6 bet that a compile-time macro is enough and no
  new interface type is needed. M4 is the test of that bet across a *core* boundary, not just
  an algorithm swap — if the session concludes the cross-core RPC needs its own abstraction
  after all, that is a legitimate finding, not a design failure, and it becomes ADR material
  either way (confirmed or revised).
- **Trailer/layout drift.** ADR-0008 D3 locked `hash[32] + signature[] + key_id` specifically
  so M4 would not need to touch the covered/excluded layout. Any impulse to widen the covered
  range or move the trailer is a signal something upstream was mis-scoped — check against D3
  before changing it.

## Blog / architecture note

The headline artifact here is **security as a service boundary, not a feature bolted onto
one core** — "the M4 that runs the attacker-reachable bootloader never holds a private key;
it asks a second core it can't equally reach over the bus" is the concrete payoff of the
asymmetric-dual-core story the roadmap has been setting up since M1. The second, quieter
artifact is **revisiting a prior architectural bet under new evidence** (ADR-0012 D6) — "I
designed a seam, and here's whether it held" is honest architecture narrative, not a
retrospective failure, regardless of which way it comes out. EP.17's "how I'd scale this to
a real multi-ECU vehicle" essay is the natural place this session's key-handling/lifecycle
discussion feeds into.
