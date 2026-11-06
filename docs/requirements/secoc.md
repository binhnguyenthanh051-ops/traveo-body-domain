# SecOC requirements — `REQ-SECOC-001..013`

Normative requirements for the A↔B Secure Onboard Communication layer. Each traces **up** to an
ADR-0021 sub-decision (the *why*) and **down** to the `@impl`/`@test` tags that prove it
(`docs/traceability-convention.md`). "shall" is normative; a requirement with no `@test` on disk
is a coverage gap the `checks/` pipeline flags. `host_tools/trace_report.py` checks the
matrix against the tags and generates `docs/traceability.md`.

**On silicon** (DoD rule 11) names the bench capture that shows the requirement on target,
as a backticked path under `docs/bench/`, or `n/a — <reason>` / `deferred — <reason>`.
`⏳` means no evidence yet, and the report counts it as a gap.

**Observed by** says *how* the behaviour is seen on the bench (CLAUDE.md rule 7, the bench-testable
rule): a contract `LOG_EVT_*` the normal image emits; bus `0xNNN` (a can_sniff capture);
`structural — <reason>`; `debugger — <what is read>`; or `n/a — <reason>`. The report fails an empty
or vague cell, an event missing from `shared/log/events.csv`, and a diagnostic event.

Design context: `docs/architecture/secoc-architecture.md`. Decisions: `ADR-0021` (protocol),
`ADR-0017` (crypto offload), `ADR-0018` (IPC transport + D6 cache), `ADR-0019` (keys),
`ADR-0002` (CAN IDs). Verdict semantics reused: `ADR-0016`.

## Coverage matrix

| ID | Requirement (short) | Traces | Verified by | On silicon | Observed by |
|---|---|---|---|---|---|
| REQ-SECOC-001 | Protected messages authenticated; unauthenticated dropped before decode | D1, D9 | `test_secoc`, `test_secoc_rx` | `docs/bench/2026-10-24/regression_r.md` (R2: 5/5 attacks dropped, no 0x120 accept) | `LOG_EVT_SECOC_REJECT_MAC` per attack frame; no `LOG_EVT_SECOC_ACCEPT` on `0x120` |
| REQ-SECOC-002 | MAC is AES-CMAC truncated to 8 B, constant-time compared | D1 | `test_secoc` | `docs/bench/2026-10-24/regression_r.md` (8 B MAC on the wire, cross-node verify; bit-flipped MAC refused). Constant-time compare: host-verified only | bus `0x121` (8 B MAC on the wire) + `LOG_EVT_SECOC_REJECT_MAC` (bit-flipped MAC) |
| REQ-SECOC-003 | Secured frame layout `PDU∥freshness(4)∥MAC(8)`, one CAN FD frame, LE wire | D2 | `test_secoc` | `docs/bench/2026-10-24/bus_round2.txt` (every secured frame: PDU ∥ epoch/ctr ∥ 8 B MAC, one CAN FD frame, padded to 16 B) | bus `0x121`, `0x200` (frame layout on the wire) |
| REQ-SECOC-004 | MAC input binds a Data ID: `CMAC(DataID∥freshness∥PDU)` | D2 | `test_secoc` | `docs/bench/2026-10-24/regression_r.md` (crossid: genuine 0x121 bytes on 0x120 → REJECT_MAC) | `LOG_EVT_SECOC_REJECT_MAC` (genuine `0x121` bytes sent on `0x120`) |
| REQ-SECOC-005 | Freshness = `epoch(u16)∥counter(u16)`; counter is per CAN-ID | D3 | `test_secoc_freshness` | `docs/bench/2026-10-24/bus_round2.txt` (Node B, 4 boots: 0x2F0 at ctr 1, then 0x200 also at ctr 1 in the same epoch) + `docs/bench/2026-10-24/nodeA_round2.txt` (Node A accepts that 0x200 ctr 1 each time) | bus `0x200`, `0x2F0` (epoch ∥ counter per CAN ID on the wire) |
| REQ-SECOC-006 | Epoch shared per-node, persisted, `++`/boot; `commit(E+1)` durable **before** spend | D3, D8 | `test_secoc_freshness`, `test_freshness_store`, `test_secoc_store` | `docs/bench/2026-10-24/epochs_power_cycle.md` (5 power cycles, epochs strictly +1 on both nodes) | bus `0x121`, `0x200` (each sender epoch +1 per power cycle) |
| REQ-SECOC-007 | Receiver accept rule (floor + per-ID high-water); MAC-check before freshness update | D4 | `test_secoc_freshness`, `test_secoc_rx` | `docs/bench/2026-10-24/g2_g3_reset_replay.md` (G2: old frame refused after a Node B reset) | `LOG_EVT_SECOC_REJECT_FRESHNESS` (an old frame replayed) |
| REQ-SECOC-008 | Receiver-reset resync via authenticated `FRESHNESS_SYNC`, both receivers (0x2F0 B→A, 0x1F0 A→B); `floor=persisted+1` | D5 + F-014 addendum | `test_secoc_resync`, `test_secoc_rx` | B-receiver: `docs/bench/2026-10-24/g2_g3_reset_replay.md` (G3: 4 Node B resets, A resyncs within 20 ms). A-receiver (0x1F0): `docs/bench/2026-11-01/f014_gateway_reboot.md` (3 Node A resets, telemetry back within 36 ms each, 0/0/1 rejects; a replayed 0x1F0 refused) | `LOG_EVT_SECOC_RESYNC` (both receiver directions) |
| REQ-SECOC-009 | Counter rollover bumps the epoch (durable) and resets counters | D3 | `test_secoc_freshness` | deferred — host-verified only. A wrap needs ~22 min of `0x200` at 20 ms; the longest capture reached ctr 34964. Capture one on the next long bench run (M5-R8) | bus `0x200` (the epoch bumps at the counter wrap; not captured yet, see On silicon) |
| REQ-SECOC-010 | `key_id`-selected key; unknown `key_id` ⇒ fail (never key 0) | D6 | `test_crypto_mac`, `test_crypto_error` | `docs/bench/2026-10-24/kat_f010.md` (a crypto failure reaches the caller as an error). Unknown key_id itself: host-verified only | `LOG_EVT_CRYPTO_ERROR` (`op` 3, `reason` 5 refused: the M0+ answers an unknown `key_id` with an error, not a tag; ADR-0023 D12) |
| REQ-SECOC-011 | Secret key resides only in the M0+ image; app uses the MAC oracle only | D6, D7 | `@design-only` + `test_crypto_mac` | `docs/bench/2026-10-30/key_residency.md` (key bytes in both CM0+ ELFs, absent from the CM4 FBL, CM4 app and CM7 app) | structural — key bytes only in the CM0+ ELFs (`host_tools/check_key_residency.py`) |
| REQ-SECOC-012 | `CRYPTO_OP_MAC=0x03` generic op: `key_id∥len∥msg ⇒ tag[16]` | D7 | `test_crypto_mac`, `test_crypto_error` | `docs/bench/2026-10-24/kat_f010.md` (op returns an error, not a tag, on a crypto fault; NIST tags on both CM0+) | `LOG_EVT_CRYPTO_ERROR` (a failed op names `op` 3 and why, e.g. `reason` 2 when the M0+ is dead; a working op shows as `LOG_EVT_SECOC_ACCEPT`) |
| REQ-SECOC-013 | Telemetry `0x200` (B→A) is authenticated on the same path | D7 | `test_secoc` | `docs/bench/2026-10-24/nodeA_round2.txt` (Node A SECOC_ACCEPT on 0x200, continuous counters) | `LOG_EVT_SECOC_ACCEPT` on `0x200` |

## Requirements

### REQ-SECOC-001 — authenticate, or drop before decode
Every message on a **protected** CAN ID shall carry a valid MAC over its secured frame; a frame
that fails the MAC check **or** the freshness gate (REQ-SECOC-007) shall be **dropped before
`body_decode`/`unpack_*`** — it shall never reach actuation — and shall increment a per-reason
drop counter (bad-MAC / stale-freshness / unknown-key). The Actuator FSM shall hold its last safe
state on a drop. *(ADR-0021 D1, D9; fail-safe mirrors ADR-0016.)*

The counters live in `secoc_rx_t` and are incremented in the same statement as the matching
`LOG_EVT_SECOC_*` event (ADR-0023 D11), which is how **REQ-LOG-009**'s "the two shall not diverge"
is satisfied. A node shall not keep a second, app-side tally of the same drops.

### REQ-SECOC-002 — CMAC, truncated, constant-time
The MAC shall be **AES-128 CMAC** (NIST SP 800-38B). The wire shall carry the **first 8 bytes** of
the 16-byte tag. The receiver shall compare the received 8 bytes against the recomputed tag's
first 8 bytes in **constant time** (no early-out on first mismatched byte). *(ADR-0021 D1.)*

### REQ-SECOC-003 — secured frame layout
A secured frame shall be `Authentic-PDU ∥ freshness(4 B) ∥ truncated-MAC(8 B)` in a **single CAN
FD frame**. `freshness` shall be `epoch(u16 LE) ∥ counter(u16 LE)`. Byte order shall be
little-endian on the wire and in the MAC input. *(ADR-0021 D2, D3.)*

### REQ-SECOC-004 — Data ID bound into the MAC
The MAC input shall be `DataID(2 B LE) ∥ freshness(4 B) ∥ Authentic-PDU`, where `DataID` is
derived from the frame's CAN ID (baseline: the 11-bit CAN ID, zero-extended). `DataID` shall
**not** be transmitted; the receiver reconstructs it from the received CAN ID. A frame replayed on
a different CAN ID shall fail verification. *(ADR-0021 D2 — closes cross-ID substitution.)*

### REQ-SECOC-005 — per-ID counter
Each protected CAN ID shall maintain its **own** freshness counter. Advancing one ID's counter
shall not cause a genuine in-range frame on another ID to be rejected as stale. *(ADR-0021 D3.)*

### REQ-SECOC-006 — epoch persistence & anti-reuse order
The `epoch` shall be **shared across all of a node's protected IDs**, persisted via the
`secoc_freshness_store` port, and incremented once per boot. On boot the sender shall
`load(E)`, then `commit(E+1)` **durably before spending E**. `commit` shall be durable before it
returns. A brown-out may waste an epoch but shall never cause an epoch value to be reused.
If a `commit` fails, the sender shall send nothing authenticated for the epoch it could not
persist (at boot, on rollover, on a resync). *(ADR-0021 D3, D8; F-015.)*

### REQ-SECOC-007 — receiver accept rule
Per remote sender the receiver shall keep one persisted `floor` (epoch) and, in RAM, one
`high-water` counter per protected ID. After a **successful** MAC check, for a frame
`(epoch, ctr)` on `id`: reject if `epoch < floor`; if `epoch > current_epoch`, adopt
(`current_epoch=epoch`, reset all high-water) ; within `current_epoch` accept iff `ctr >
high-water[id]`, then set `high-water[id]=ctr`. The MAC check shall occur **before** any freshness
state is updated, so a forged frame cannot advance high-water. A new epoch shall be persisted
**before** it is adopted; if that `commit` fails, the frame shall be dropped as stale and no
freshness state shall change. *(ADR-0021 D4; F-015.)*

### REQ-SECOC-008 — receiver-reset resync
On boot **each** receiver shall raise `floor = persisted_epoch + 1` (rejecting the entire prior
epoch) and shall send an **authenticated** `FRESHNESS_SYNC` carrying its new floor: Node B on CAN
ID **`0x2F0`** (B→A, for the command epoch), Node A on **`0x1F0`** (A→B, for the telemetry epoch).
On receipt the peer shall set its sender `epoch = max(current, requested_floor)`. `FRESHNESS_SYNC`
shall itself be a protected frame (REQ-SECOC-001..004) so a replayed or forged sync is rejected.
*(ADR-0021 D5 and its F-014 addendum, 2026-10-30; ADR-0002 SecOC-management sub-band extension.)*

### REQ-SECOC-009 — counter rollover
When a sender's per-ID counter would advance past `0xFFFF`, the sender shall instead bump the
epoch (`commit(E+1)` durably, reset all counters to 0) rather than wrap the counter. *(ADR-0021
D3.)*

### REQ-SECOC-010 — key selection, unknown ⇒ fail
The MAC key shall be selected by `key_id`. An unknown `key_id` shall be a **failure** (MAC op
returns error ⇒ drop), never a fallback to key 0. `key_id == 0` shall never name a valid secret.
*(ADR-0021 D6; ADR-0019 D3.)*

### REQ-SECOC-011 — key residency (design-only for the residency itself)
The AES-CMAC secret shall reside **only** in each node's M0+ image; the application core shall hold
no key bytes and shall obtain MACs solely via the `CRYPTO_OP_MAC` oracle. The *residency* is
target composition (`@design-only`); the *oracle-only* app contract is host-verified (the app path
never takes key bytes as input). *(ADR-0021 D6, D7; ADR-0017 reason 1; ADR-0020.)*

### REQ-SECOC-012 — the generic MAC op
`CRYPTO_OP_MAC = 0x03` shall be a SecOC-agnostic op. Request payload: `key_id(u32 LE) ∥
msg_len(u16 LE) ∥ msg[msg_len]`; response payload: `tag[16]`. It shall MAC opaque bytes only —
freshness, Data ID, and truncation are the caller's concern. An unknown `key_id` or malformed
request shall yield an error verdict, not a tag. *(ADR-0021 D7; ADR-0017 D3 dispatch.)*

### REQ-SECOC-013 — telemetry authenticated
Telemetry on CAN ID **`0x200`** (B→A) shall be authenticated on the identical secured-frame path;
Node A shall apply REQ-SECOC-007 against Node B's sender epoch. *(ADR-0021 D7.)*
