# ADR-0021: SecOC protocol — framing, freshness, resync, and symmetric-key handling (M5)

**Status:** accepted · **Date:** 2026-07-25

> Owns the **SecOC protocol** for the A↔B control network: the secured-frame layout, the MAC
> primitive and its truncation, the freshness scheme (per-ID counter over a shared per-boot
> epoch), the receiver accept rule, the receiver-reset resync, and the symmetric-key handling.
> It sits on top of the crypto-service offload (ADR-0017), reuses the IPC transport unchanged
> (ADR-0018, incl. the D6 non-cacheable-mailbox addendum for Node B), extends the key model
> (ADR-0019) with a symmetric secret, and extends the CAN-ID scheme (ADR-0002). The node
> software integration is `docs/architecture/secoc-architecture.md`; the requirements it must
> satisfy are `REQ-SECOC-001..013` in `docs/requirements/secoc.md`; `@impl`/`@test` tags link
> code and tests back to the `Dn` here.

## Context

M4 built a signature-verification service to protect **boot** (is this app image authentic?).
M5 protects **runtime traffic**: an on-bus attacker who can inject/replay CAN FD frames must not
be able to command the actuator. This is a different threat surface from ADR-0017 D5 — that
boundary was on-chip (M4↔M0+) and needed *no* anti-replay; **this** boundary is the physical bus
and needs authentication **and** freshness. Naming when you do vs don't need replay protection is
half the point: same word ("message"), opposite conclusion.

The primitive is a **symmetric MAC** (AES-CMAC), not a signature: both nodes share a secret and
either can produce a tag. That is the honest, stated limitation of SecOC — any key-holder can
forge — and it is why the secret is offloaded to the M0+ (ADR-0017 reason 1, the "real reason"
the service exists at all): a memory bug in the attacker-reachable app must not be able to *read*
the key, even though a fully-compromised app could still *call* the MAC oracle. We protect the
key from disclosure, never the actuation decision. Node B has **no app secure boot** in M5
(secoc-architecture §8.2), so this residual is real and named, not hidden.

CAN FD's 64-byte frame is the enabler: payload + freshness + truncated MAC ride in **one** frame,
so we avoid every Classic-CAN SecOC contortion (secondary MAC PDUs, on-bus freshness truncation
and reconstruction).

## Decisions

### D1. MAC primitive: AES-CMAC, computed on the M0+, truncated to 8 bytes, compared constant-time

- **Algorithm:** AES-128 CMAC (NIST SP 800-38B). Vetted primitive, no hand-rolled subkey/padding
  (ADR-0006). On target it is the PDL's **Crypto Core V2 CMAC**, called step by step
  (`Cy_Crypto_Core_Cmac_Init/Start/Update/Finish/Free`) with the Crypto block's error flags checked
  after, not through the one-shot `Cy_Crypto_Core_Cmac`, which cannot report a failure (F-010, see the
  addendum below); `CY_IP_MXCRYPTO` is present on `cyt4bf8cds.h` (Node B) and on CYT2B7 (Node A). mbedTLS
  `mbedtls_cipher_cmac` is the documented software fallback, not expected. On-silicon correctness
  is proven against SP 800-38B known-answer vectors at bring-up.
- **Truncation:** the full CMAC tag is 16 B; the wire carries the **first 8 B**. 64-bit forgery
  resistance is generous for a body-domain command rate — an attacker gets one online guess per
  injected frame (no offline oracle; the tag depends on freshness that advances), so 2⁻⁶⁴ per
  attempt is far past sufficient. We are not bandwidth-starved on CAN FD, so we do **not** shrink
  further toward AUTOSAR's aggressive 24–28-bit truncations (those exist to fit Classic CAN's 8 B).
- **Where truncation + compare live:** the M0+ op is a **generic "AES-CMAC over these bytes →
  16-B tag"** RPC (D7). Truncation to 8 B and the **constant-time** equality check happen in
  `shared/secoc` on the app core — host-testable, and keeping the op generic means the M0+ never
  learns SecOC's framing (layer discipline, ADR-0017 D3). Constant-time compare is hygiene: it
  denies a timing oracle on the compare even though the dominant guard is freshness.
- **Tradeoff / alternative:** HMAC-SHA256 truncated. Rejected — CMAC reuses the AES block the HW
  accelerates, tags are naturally 16 B, and CMAC is the automotive-conventional SecOC MAC.

### D2. Secured frame layout — and the MAC input binds a **Data ID** (proposed strengthening of the locked format)

On-wire, one CAN FD frame:

```
┌───────────────────────┬──────────────────────────┬────────────────────────┐
│ Authentic PDU (P B)   │ Freshness (4 B)          │ Truncated MAC (8 B)    │
│  e.g. DOOR_UNLOCK      │ epoch(u16 LE)∥ctr(u16 LE)│  CMAC[0..8)            │
└───────────────────────┴──────────────────────────┴────────────────────────┘
Transmitted = PDU ∥ freshness(4 B) ∥ MAC[0..8)
MAC input   = DataID(2 B LE) ∥ freshness(4 B) ∥ PDU        ← DataID is NOT transmitted
```

- **Endianness:** both `epoch` and `ctr` are **little-endian** on the wire and in the MAC input.
  Fixed here so the two nodes (and host tests) cannot disagree silently.
- **Data ID in the MAC input — the change vs the locked "MAC over freshness∥PDU":** the MAC input
  is prefixed with a 2-byte **SecOC Data ID** derived from the CAN ID of the frame (baseline: the
  11-bit CAN ID itself, zero-extended). It is an **input to the CMAC, not transmitted** — the
  receiver already knows which ID it received the frame on and reconstructs the same input.
  - **Why it is not optional.** Without it, the shared key (D6) + independent per-ID counters over
    a shared epoch (D3) let an attacker take a valid frame on ID *X* and replay its bytes on ID
    *Y*: the MAC still verifies (same key, same MAC'd bytes) and the freshness passes iff *Y*'s
    high-water is below *X*'s counter. Concretely, a captured `0x120 DOOR_UNLOCK` replays as a
    `0x121` command. Binding the Data ID makes the tag ID-specific and closes the substitution —
    this is exactly why AUTOSAR SecOC feeds a Data ID into the MAC.
  - **Cost:** zero on-bus bytes, 2 bytes of MAC input, one config field per protected ID.
  - **Alternative considered:** rely on distinct freshness *values* per ID to make substitution
    "unlikely." Rejected — it is not a security property, only a probability, and it breaks the
    moment two IDs' counters are close (which is the normal case just after a shared epoch bump).
- **Overhead:** freshness (4) + MAC (8) = **12 B** added per protected frame; a 1-byte command
  becomes 13 B, still one 16-B CAN FD DLC. Well inside 64 B.

### D3. Freshness = `epoch(u16) ∥ counter(u16)` — per-ID counter, shared per-boot epoch

- **`counter` is per CAN-ID.** Each protected ID keeps its own u16 RAM counter, so a genuine,
  slightly-reordered frame on ID *Y* is never rejected as stale because ID *X* advanced. Sender
  increments `ctr[id]` per frame; the value transmitted is the pre-increment (spent) value.
- **`epoch` is per node, shared across all that node's IDs, persisted, `++` per boot.** Because it
  is shared, per-ID counters do **not** multiply flash writes — still **one persist per boot**
  regardless of ID count. Persist via the D8 store.
- **Persist order (the correctness invariant):** on boot `load(E)` → `commit(E+1)` **durably,
  before spending E** → then use `E` for outgoing frames. A brown-out can *waste* an epoch, never
  *reuse* one. (Matches the ADR-0008 "commit before spend" instinct.)
- **Counter rollover within an epoch (new, was unspecified):** when a sender's `ctr[id]` would
  wrap past `0xFFFF`, the sender **bumps the epoch** (`commit(E+1)` durably, reset all `ctr[*]` to
  0) rather than wrapping the counter. The receiver adopts the new epoch by the D4 rule for free.
  Consequence, stated honestly: persistence is "one write per boot **plus** one per (rare)
  rollover," not strictly one per boot. At a 100 ms cyclic rate a wrap is ~1.8 h away, so this is
  a corner, but it is a *defined* corner.
- **Epoch width (stated limitation):** u16 epoch = 65 536 boots. Over a 15-year life at ~10
  starts/day (~55 k) that is on the edge, and rollover-driven bumps (above) spend from the same
  space. Baseline accepts u16 with a **defined wrap-to-0 behavior** (the receiver's floor logic in
  D4 treats it as any other adoption; a wrap that crosses an un-synced receiver is closed by the
  D5 sync). **Alternative:** widen freshness to u32 epoch (or a full u48/u64 monotonic counter as
  real fleets do). Rejected for M5 — costs bytes and a wider store for a limit we won't hit on the
  bench; recorded as design-essay scope, not built.

### D4. Receiver accept rule — a persisted **floor** plus per-ID RAM **high-water**

Per remote sender the receiver keeps one persisted `floor` (epoch) and, in RAM, one high-water
`hw[id]` (counter) per protected ID. On a frame `(epoch, ctr)` for `id`, after the MAC verifies:

- `epoch <  floor`              → **reject** (stale / pre-reset replay).
- `epoch >  current_epoch`      → **adopt**: `current_epoch = epoch`, reset all `hw[*] = 0`, then
  accept iff `ctr > 0`-relative below.
- within `current_epoch`: accept iff `ctr > hw[id]`, then `hw[id] = ctr`. (Strictly increasing ⇒
  duplicates and replays are dropped; gaps from lost frames are tolerated.)

The **floor** is the single persisted receiver value (D8). High-water is RAM-only — deliberately,
so a receiver reboot *loses* it (that loss is precisely what D5 must cover). MAC-check happens
**before** the freshness update so a forged frame can never advance `hw[id]` (no freshness-DoS via
bad MACs).

### D5. Receiver-reset resync — authenticated `FRESHNESS_SYNC` (0x2F0, B→A), included in M5

The asymmetry that makes this necessary:

- **Sender reboot self-heals.** A rebooting *sender* (A) comes up at `persisted+1` — a *higher*
  epoch — so its next command trips the D4 "adopt" branch on B. No message needed.
- **Receiver reboot does not.** A rebooting *receiver* (B) loses its RAM high-water. If it set
  `floor = persisted`, an attacker could replay a captured in-epoch frame (high-water is back to
  0). So B raises `floor = persisted+1`, rejecting the **entire** old epoch — which also rejects
  the *legitimate* still-running sender A (A is still at the old, now-too-low epoch). Deadlock.

`FRESHNESS_SYNC` breaks it: on boot B sends an **authenticated** frame on `0x2F0` (B→A) carrying
its new floor; A adopts `epoch = max(current, requested_floor)` and its commands are accepted
again. Because the sync is itself MAC'd with B's freshness on `0x2F0`, a replayed old sync is
rejected by A's high-water for `0x2F0`, and an attacker cannot forge a floor bump.

- **Bootstrap:** first-ever contact has both at `epoch=1` (`persisted 0 → +1`); nothing special.
- **Tradeoff / alternative:** a challenge-response nonce handshake (B challenges, A signs). Stronger
  (no reliance on B's own persisted floor) but adds a round trip and a second message pair, and
  buys nothing against M5's threat because the floor is authenticated. Rejected for M5; noted.

### D6. Symmetric key — one shared secret, `key_id`-selected, compiled into each node's M0+ image

- The AES-CMAC secret is **shared** between A and B and lives **only in the M0+ image** of each
  node (ADR-0019 D2 pattern extended from public→secret material; ADR-0020 keeps it in the M0+
  TCB). The app never holds key bytes; it calls the MAC oracle.
- `crypto_keystore` gains an **AES-secret key type** alongside the ECDSA public entry. `key_id`
  selects it (ADR-0019 D3): an **unknown `key_id` is a failure**, never "default to key 0" — an
  attacker must not name away the real key. Key 0 is never a valid secret.
- **Honest limitation (secoc-architecture §8.3):** real fleets provision **per-ECU** keys from a
  backend key master via a KDF; one shared secret is portfolio scope, stated plainly. `key_id`
  leaves the door open to per-link keys without a protocol change.
- **Alternative:** per-direction keys (A→B distinct from B→A). Marginally better (a compromised
  telemetry path can't forge commands) and *almost free* here — two `key_id`s instead of one.
  Recorded as a cheap future hardening; M5 ships one key to keep the bring-up surface minimal.
- **Future milestone — session keys.** A larger evolution replaces the static shared secret with
  **per-session derived MAC keys** (a key-agreement/derivation handshake + rekey lifecycle, and a
  freshness scheme that resets per session). It changes the trust model enough to warrant **its own
  milestone and ADR arc**, not an M5 add-on — but it reuses these seams unchanged (the generic
  `CRYPTO_OP_MAC` op, `key_id` selection, the freshness-store port). Out of scope for M5; noted so
  the static-key baseline is explicitly a *baseline*, not the end state.

### D7. The crypto op is generic AES-CMAC-over-bytes (`CRYPTO_OP_MAC = 0x03`); telemetry is authenticated

- Fills the **reserved** `CRYPTO_OP_MAC = 0x03` slot in the ADR-0017 dispatch table. Request
  payload: `key_id(u32 LE) ∥ msg_len(u16 LE) ∥ msg[msg_len]`; response payload: `tag[16]`. The op
  is deliberately **SecOC-agnostic** — it MACs opaque bytes and returns 16 B; freshness, Data ID,
  and truncation are the caller's concern (D1/D2). `crypto_msg` gains MAC request/tag helpers;
  `crypto_service` gains a `crypto_mac()` sibling of `crypto_verify_image()`.
- **`msg` must fit** `CRYPTO_MAX_PAYLOAD` (128 B) minus the `key_id`+`len` header — comfortable for
  `DataID ∥ freshness ∥ PDU` on a body network. A larger PDU is a `CRYPTO_MAX_PAYLOAD` bump, not a
  protocol change.
- **Telemetry `0x200` (B→A) is authenticated (locked D7).** It rides the identical path; A is its
  receiver and applies D4 against B's sender epoch. No separate ADR — symmetric-key handling for
  every protected ID lives here.

### D8. Freshness persistence port — `secoc_freshness_store_if_t`, durable-before-return

```c
typedef struct {
    bool (*load)(uint8_t domain, uint16_t *epoch);   /* false ⇒ no stored value (cold) */
    bool (*commit)(uint8_t domain, uint16_t epoch);  /* MUST be durable before it returns */
} secoc_freshness_store_if_t;
```

- `domain` selects the persisted value (a node's own sender epoch; a receiver's floor per remote).
- **`commit` is durable-before-return** — the whole D3/D5 anti-reuse argument rests on it. A store
  that buffers a write and returns is a correctness bug, not an optimization.
- **Host fake now** (a volatile map with a settable "power-cycle" to test cold boot and
  brown-out-between-commit-and-spend); **`eeprom_emu` backing in M6**. Target-only, behind the
  port — `shared/secoc` never sees flash (the `secoc_core` layer rule in `checks/rules.yml`).
- **Backing revised (2026-10-20, M5B):** the target backing is now a dedicated two-slot record in
  work flash (**ADR-0024**), not `eeprom_emu` in M6. The port itself is unchanged.

### D9. Verify failure is fail-safe: drop, count, hold last safe state

A frame that fails the MAC check **or** the D4 freshness gate is **dropped before decode** — it
never reaches `body_decode`/`unpack_*`, so it cannot actuate. The receiver increments a
per-reason counter (bad-MAC vs stale-freshness vs unknown-key, for health/telemetry) and the
Actuator FSM **holds its last safe state**. This mirrors the ADR-0016 boot fail-safe: an
unauthenticated command is treated exactly like no command. `INVALID`/`ERROR` from the M0+ both
map to drop (ADR-0017 verdict semantics).

## Host/target split (ADR-0001)

| Host-testable core (GCC + Unity) | Target-only (behind ports) |
|---|---|
| frame pack/unpack + Data-ID MAC-input assembly (D2) | HW AES-CMAC back end on the M0+ (D1) |
| composed RX verdict + per-reason counters + `LOG_EVT_SECOC_*` emission (`secoc_rx.c`, ADR-0023 D11) | the UART sink behind `log_port_*` (ADR-0023 D8) |
| truncation + constant-time compare (D1) | the **secret key bytes** in the M0+ image (D6) |
| freshness gen + accept rule + rollover + adopt (D3/D4) | `secoc_freshness_store` `eeprom_emu` backing (D8, M6) |
| resync floor logic (D5) | Node B mailbox cache attribute (ADR-0018 D6) |
| `CRYPTO_OP_MAC` framing + dispatch row (D7) | CANFD RX ISR / `can_hal` on each BSP |
| `secoc_freshness_store` **fake** (D8) | — |

The M4→M7 silicon change (secoc-architecture §9) touches **only** the right column.

## Consequences

- (+) One CAN FD frame carries payload+freshness+MAC — no secondary-PDU machinery (D2).
- (+) Data-ID binding closes cross-ID substitution at zero on-bus cost (D2) — the one place the
  locked format had a real hole.
- (+) Per-ID counter over a shared epoch gives correct anti-replay at **one flash write per boot**
  (D3), with a *defined* rollover corner rather than a lurking one.
- (+) The MAC op reuses the ADR-0017 dispatch unchanged — additive row, the payoff of building the
  service once (D7).
- (+) Fail-safe by construction: unauthenticated ⇒ dropped-before-decode (D9).
- (−) Symmetric shared key: any key-holder can forge; a compromised Node B app can still call the
  oracle (D6, §8.2). Inherent to symmetric SecOC, named not hidden.
- (−) Per-frame M0+ round trip on both TX and RX (D1/D7) — µs of CMAC, dominated by IPC latency;
  a control-budget **verify-on-silicon** item (§8.4).
- (−) u16 epoch is life-marginal (D3) and persistence is per-boot **plus** per-rollover — both
  documented, neither hit on the bench.

## Alternatives considered

- **MAC over `freshness ∥ PDU` (no Data ID)** — the locked baseline; rejected (D2), cross-ID
  substitution under a shared key + per-ID counters.
- **HMAC-SHA256** MAC — rejected (D1); CMAC reuses the AES block and is the SecOC convention.
- **24–28-bit MAC truncation** (AUTOSAR/Classic-CAN) — unnecessary on CAN FD; 8 B for margin (D1).
- **Global (not per-ID) counter** — one counter shared by all IDs; rejected (D3), reorders across
  IDs become false stale-drops.
- **Nonce challenge-response resync** — stronger, extra round trip; rejected for M5 (D5).
- **Per-direction / per-ECU keys** — better isolation; per-direction recorded as cheap future
  hardening, per-ECU is real-fleet scope (D6).

## To verify in the TRM / on silicon

- ✅ `Cy_Crypto_Core_V2_Cmac` KAT against SP 800-38B on both parts (D1): exact on CYT2B7 and
  CYT4BF, no byte reversal, no explicit `Aes_Init` (2026-09-28, runbook #9). Both parts compute the
  same tag with the shared secret (Stage 4.1). MXCRYPTO is present on CYT4BF (the V2 driver links).
- ✅ F-010 fix on silicon (D1): `g_kat_result == 15` on both parts. Bit 3 shows an injected Crypto
  bus error makes the next MAC fail, and the one after it is correct (2026-10-24,
  `docs/bench/2026-10-24/kat_f010.md`).
- **Deferred** (2026-10-25): per-frame MAC round-trip latency vs the control-loop budget (D1/D7,
  §8.4) is **not measured**. Upper bound only: both nodes sustain one secured frame each way every
  20 ms with zero drops (Stage 5, and the M5B bench on 2026-10-24). A timed measurement needs an
  emitter for `LOG_EVT_CRYPTO_MAC_US` (defined, unused). This is a design open item, not a REQ, so
  it doesn't block M5B. Pick it up with the profiling work.
- ✅ Node B mailbox non-cacheable region (ADR-0018 D6): verified with the D-cache on (runbook #12).
- ⏳ `secoc_freshness_store` durability semantics once `eeprom_emu` backs it (D8, M6).

## Review history

Drafted this session from the locked decisions in `docs/architecture/secoc-architecture.md`.
Three items were surfaced for the owner beyond the locked set and **accepted** (2026-07-25):
**D2**'s Data-ID addition to the MAC input (changes the locked "MAC over freshness∥PDU" format —
closes cross-ID substitution at zero on-bus cost); **D3**'s counter-rollover epoch-bump (fills
the previously-undefined wrap corner); and the **u16-epoch** width kept as-is with documented
wrap behavior (u32 recorded as design-essay-scope alternative). Requirements
(`docs/requirements/secoc.md`, `REQ-SECOC-001..013`) and the failing Unity suite (`test_secoc`,
`test_secoc_freshness`, `test_crypto_mac`, `test_secoc_resync`, `test_freshness_store`) land
against these `Dn`.

**Implementation landed (2026-07-26).** The host-testable core is built and **CI-green** across
all five suites: `shared/secoc/secoc.c` (frame + Data-ID MAC input + 8-B truncation +
constant-time compare, D1/D2/D9), `shared/secoc/secoc_freshness.c` (per-ID counter, rollover
epoch-bump, floor/high-water accept rule, resync floor, D3/D4/D5/D8), the `CRYPTO_OP_MAC` framing
in `shared/crypto/crypto_msg.c` (D7), the `crypto_keystore` AES-secret type (D6), and the
`crypto_mac()` client in `shared/crypto/crypto_service.c` (D7). `@impl` tags link each `Dn`/`REQ`
to code; `@test` tags (added by Copilot) close the ADR↔test / REQ↔test matrix.

The **target seams are authored but bench-pending** (target-only, not host-built): the
`CRYPTO_OP_MAC` AES-CMAC handler + shared-secret keystore in both CM0+ images
(`crypto_ops_cm0p.c`, Node A + Node B; `security/include/secoc_shared_secret.h`), the Node B CM0+
mailbox server loop and its non-cacheable mailbox reservation in both linker scripts, the CM7
`ipc_port_if_t` binding + **MPU non-cacheable region** (ADR-0018 D6), and the `secoc_app` +
actuator-FSM integration wiring authenticated commands/telemetry/resync into both nodes' CAN
paths. Open silicon-verify items — the PDL CMAC symbol names + NIST SP 800-38B KAT (D1), the MPU
region vs BSP, the CAT1C IPC channel, the cross-file mailbox-address invariant, and per-frame
MAC latency vs the control budget — are consolidated for bench bring-up; findings append here as
in ADR-0017/0018. M6 swaps the RAM freshness store for `eeprom_emu` (D8) for real cross-reboot
anti-replay.

**Addendum — D2 on a real bus: the receiver uses a configured PDU length (2026-10-03, W40 Stage 5).**
The first real-bus capture showed every secured frame arriving **16 B**, not 13/14/15 B: CAN FD has
no 13..15-byte data length, so the controller pads to the next valid one (12, 16, 20, 24, 32, 48,
64). `secoc_verify` inferred the PDU as `frame_len − 12`, which on a padded frame moves the MAC
window onto the wrong bytes, so **every genuine frame would have been rejected as `BAD_MAC`**.
Host tests (exact lengths) and Phase A loopback could not show it.
*Decision (option B, owner-approved):* the receiver takes the authentic PDU length **per CAN ID
from configuration** (`body_msg_pdu_len()` in `shared/messages`: 0x120/0x121 = 1, 0x2F0 = 2,
0x200 = 3), as AUTOSAR SecOC configures the authentic I-PDU length rather than deriving it from
the DLC. `secoc_verify_len()` accepts exactly PDU + 12 or that length padded to the next CAN FD
length, rejects anything else as `SECOC_BAD_LENGTH`, and ignores the padding bytes, which are not
part of the authentic I-PDU and not under the MAC. `secoc_rx` binds the table through an optional
`pdu_len_of` lookup set by each node's `secoc_app`. SecOC still names no message IDs, and an ID
the table does not know keeps the previous inference path, so the unregistered-ID verdict
(freshness drop) is unchanged. The on-wire layout of D2 is unchanged.
*Alternatives:* (A) sender pads the PDU so PDU + 12 is a valid FD length. No receiver change,
but the format then depends on the DLC table by accident, the padding falls under the MAC, and
the next message size breaks it again. (C) trailer first (freshness ∥ MAC ∥ PDU), which is
non-standard, and the receiver still needs the PDU length to bound decoding.
*Evidence:* `test_secoc` + `test_secoc_rx` gain 8 cases, `test_body_msgs` 1 (`body_msg_pdu_len`
matches the packers). The full host suite went from 230 to 239 tests, all passing. Bench capture:
`docs/bench/2026-10-03/bus_node_b.txt`.

**Addendum — D1 on silicon: the PDL's one-shot CMAC cannot report failure (2026-09-28, F-010).**
`Cy_Crypto_Core_V2_Cmac` in mtb-pdl-cat1 3.22.1 tracks a status through Init/Start/Update/Finish,
then returns `CY_CRYPTO_SUCCESS` unconditionally. Both parts use the V2 driver, so `cmac_compute()`
on the M0+ can never see a crypto-hardware failure, and `mac_handler` would answer with a tag
computed from whatever the hardware left, instead of the ERROR verdict REQ-SECOC-010 requires. It
**stays fail-safe at the receiver**: a wrong tag fails verification and the frame is dropped
(REQ-SECOC-001). But on the sender it is a silent wrong MAC rather than a reported fault, which is
not what D1 claims. *Planned fix (F-010):* call `Cy_Crypto_Core_V2_Cmac_Init/Start/Update/Finish`
directly and check each status. Until then the NIST KAT (Stage 2) is the evidence that the
primitive works on this silicon.

**Addendum — D1 fix: step-by-step CMAC plus a hardware error check (2026-10-22, F-010).**
Reading the PDL source for the planned fix showed that checking each step's status is **not enough
on its own**. In mtb-pdl-cat1 3.22.1 (Node A) and 3.23.0 (Node B, same CMAC code) the steps fail
only on bad parameters: `Cmac_Init` on a NULL pointer, the multi-step `Cy_Crypto_Core_Cmac_*` on a
V1 block (`NOT_SUPPORTED`). `Aes_Init` (inside `Start`), `Aes_Ecb` (inside `Update`) and `Finish`
return `CY_CRYPTO_SUCCESS` on every path. A hardware fault shows only in the Crypto block's `INTR`
error bits (bus error, instruction opcode/condition-code, TRNG health: `CY_CRYPTO_INTR_ERROR_MASK`),
which is where the PDL's own crypto server looks (`Cy_Crypto_Server_ErrorHandler`). We do not run
that server: our M0+ uses its own dispatch (ADR-0017), so nothing else reads those bits.
The one-shot also never calls `Cmac_Free`, so the subkeys stay on the M0+ stack.
*Decision:* `cmac_compute()` on both M0+ images calls the public multi-step API
`Cy_Crypto_Core_Cmac_Init/Start/Update/Finish`, checks each status, always calls `Cmac_Free` after
a good `Init`, then reads `INTR & CY_CRYPTO_INTR_ERROR_MASK`. Any error bit fails the MAC
(`CY_CRYPTO_HW_ERROR` → `mac_handler` answers with the ERROR verdict, REQ-SECOC-010) and is
cleared, so one fault costs one frame, not every frame after it. A bit left over from an earlier
operation (Node A's boot-time SHA/ECDSA) fails the next MAC once, which is fail-closed.
*Tradeoffs:* the state and buffer types (`cy_stc_crypto_v2_cmac_state_t/_buffers_t`) are marked
`\cond INTERNAL` in the PDL docs, but the public multi-step functions take them as `void *` and
there is no public alternative. The PDL is pinned per node, and a PDL bump must re-run the KAT.
`Cmac_Free` in these versions clears `k` twice and never clears `temp` (it reads `cmacState->k`
for both), so the last CBC chaining block stays on the stack. That block is not key material, and
finishing a MAC from it still needs the AES key, which `Free` does clear. We use the PDL as is
and do not patch `mtb_shared`.
*Alternatives:* (A) keep the one-shot and only read the error bits after it. That is smaller, but
it still loses the step statuses and the scrub. (B) Use the `Cy_Crypto_Core_V2_Cmac_*` functions
directly, as the plan said. They are the same code without the V1/V2 dispatch, so a wrong block
would run V2 code and not fail with `NOT_SUPPORTED`. (C) mbedTLS software CMAC, the D1 fallback.
It reports errors properly, but it moves AES off the accelerator and puts more code on the
trusted core for no gain in correctness.
*Evidence:* target-only code, so there are no host tests (the host fake `secoc_mac_fake.h` already
covers the ERROR path above the M0+). The `CRYPTO_BRINGUP_KAT` image gains bit 3 (`KAT_FAULT_OK`):
it raises `INTR_SET.BUS_ERROR` the way a fault would, needs the next MAC to **fail** and the one
after to give the NIST D.1 tag, so the pass value moves from 7 to **15**. The silicon capture is
planned for the W43 Saturday bench (`docs/bench/2026-10-24/`).

**M5 bench closed (2026-10-03).** All stages passed on silicon: Stage 2 (KAT), Stage 3 (offload with a
live D-cache), Stage 4 (same tag on both parts), Stage 5 (real bus both ways, gap-free counters, zero
rejects, courtesy light end to end), and **Stage 6: forged, bit-flipped, replayed and cross-ID frames
each refused with exactly one event, and the door never unlocked** (runbook #9–#20). Stage 7 (resync)
cannot trigger naturally with the RAM-backed store (see the runbook) and is left for M6, when
`eeprom_emu` makes the floor persistent.


**Addendum — D5 in both directions: Node A sends its own boot sync (2026-10-30, F-014 / M5-R4).**
D5's argument ("a receiver reboot does not self-heal") holds for **every** receiver, but D5 only
built it for Node B. Node A is also a receiver, of telemetry `0x200`. With ADR-0024's persistent
floor, a Node A reboot on its own raises A's telemetry floor to persisted + 1 while Node B keeps
sending at its old epoch, so every `0x200` is `REJECT_FRESHNESS` until B happens to reboot (460 in a
row on the 2026-10-24 bench, `docs/bench/2026-10-24/regression_r.md`). M5's RAM store hid this,
because a cold receiver accepted any epoch.
*Decision (option 1 in F-014, owner-approved 2026-10-30):* the mirror of D5, A → B.
- **New ID `0x1F0` `GW_FRESHNESS_SYNC` (A → B).** It is the top of ADR-0002's gateway → actuator
  band, as `0x2F0` is the top of the actuator → gateway band. PDU = 2 B, A's new telemetry floor,
  little-endian, the same layout as `0x2F0`. `body_msg_pdu_len(0x1F0) = 2`.
- **Node A** boots its `0x200` receiver as today (floor = persisted + 1, ADR-0024), adds `0x1F0` to
  its sender IDs, and sends one authenticated sync from `can_task` at startup, as B does today.
- **Node B** adds `0x1F0` to its receiver IDs. On an ACCEPTed sync it calls the existing
  `secoc_rx_sync_adopt()` on its telemetry sender: epoch = max(current, requested_floor), committed
  durably before it is spent (D3), all per-ID counters reset. `LOG_EVT_SECOC_RESYNC` now fires on
  whichever node completes a resync. `arg0` (the sync's CAN ID) tells the two directions apart.
- **No change in `shared/secoc`.** The adopt path is direction-agnostic already; this is node wiring,
  one message ID and the PDU-length table.
*Why a replayed sync is refused:* the sync is a protected frame under A's freshness on `0x1F0`. A
sends it at the new epoch it has just committed, so B adopts that epoch (D4). A sync captured
from an earlier A boot carries a lower epoch, which D4 rejects as stale, so an attacker can't
force B's epoch forwards or backwards. That is the D5 argument, unchanged.
*Residual, stated honestly:* like D5, the sync is sent once and not acknowledged by the peer app.
On this 2-node bus a successful `can_tx` means B's controller acked the frame, so the sync can only
be lost in B's software (RX queue full while B is busy). If that happens, telemetry stays
deadlocked until the next A or B reboot. It is accepted for now; the cheap fix, if a bench run ever
shows it, is for A to re-send the sync after N consecutive `REJECT_FRESHNESS` on `0x200`.
*Alternatives:* (2) A boots its telemetry receiver at floor = persisted, not + 1. No new message,
but after an A reboot any `0x200` captured in B's current epoch replays into A, so REQ-SECOC-007 is
deliberately weakened for telemetry. Telemetry drives the courtesy light, not the door, so the
harm is small, but "authenticated but replayable" is a hard claim to defend in a security post.
(3) A documented known limitation. That is honest, but it leaves a real deadlock in a milestone
whose whole point is the persistent store. (4) Nonce challenge-response, which D5 already rejected
for M5, for the same reason.
*Requirement:* REQ-SECOC-008 now covers both receivers (`docs/requirements/secoc.md`).
*Evidence (planned, W45):* `test_secoc_resync` gains the A-reboot direction (B's sender adopts A's
floor, telemetry is accepted again) and a replayed `0x1F0` from an earlier A epoch refused;
`test_body_msgs` checks `body_msg_pdu_len(0x1F0)`. Silicon: the F section of the W44 bench
checklist (re-dated to W45), with three A-only resets and a replayed `0x1F0` refused.
