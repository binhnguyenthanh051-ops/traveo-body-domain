# SecOC requirements — `REQ-SECOC-001..013`

Normative requirements for the A↔B Secure Onboard Communication layer. Each traces **up** to an
ADR-0021 sub-decision (the *why*) and **down** to the `@impl`/`@test` tags that prove it
(`docs/traceability-convention.md`). "shall" is normative; a requirement with no `@test` on disk
is a coverage gap the `checks/` pipeline flags.

Design context: `docs/architecture/secoc-architecture.md`. Decisions: `ADR-0021` (protocol),
`ADR-0017` (crypto offload), `ADR-0018` (IPC transport + D6 cache), `ADR-0019` (keys),
`ADR-0002` (CAN IDs). Verdict semantics reused: `ADR-0016`.

## Coverage matrix

| ID | Requirement (short) | Traces | Verified by |
|---|---|---|---|
| REQ-SECOC-001 | Protected messages authenticated; unauthenticated dropped before decode | D1, D9 | `test_secoc`, `test_secoc_rx` |
| REQ-SECOC-002 | MAC is AES-CMAC truncated to 8 B, constant-time compared | D1 | `test_crypto_mac`, `test_secoc` |
| REQ-SECOC-003 | Secured frame layout `PDU∥freshness(4)∥MAC(8)`, one CAN FD frame, LE wire | D2 | `test_secoc` |
| REQ-SECOC-004 | MAC input binds a Data ID: `CMAC(DataID∥freshness∥PDU)` | D2 | `test_secoc` |
| REQ-SECOC-005 | Freshness = `epoch(u16)∥counter(u16)`; counter is per CAN-ID | D3 | `test_secoc_freshness` |
| REQ-SECOC-006 | Epoch shared per-node, persisted, `++`/boot; `commit(E+1)` durable **before** spend | D3, D8 | `test_secoc_freshness`, `test_freshness_store` |
| REQ-SECOC-007 | Receiver accept rule (floor + per-ID high-water); MAC-check before freshness update | D4 | `test_secoc_freshness`, `test_secoc`, `test_secoc_rx` |
| REQ-SECOC-008 | Receiver-reset resync via authenticated `FRESHNESS_SYNC` (0x2F0); `floor=persisted+1` | D5 | `test_secoc_resync`, `test_secoc_rx` |
| REQ-SECOC-009 | Counter rollover bumps the epoch (durable) and resets counters | D3 | `test_secoc_freshness` |
| REQ-SECOC-010 | `key_id`-selected key; unknown `key_id` ⇒ fail (never key 0) | D6 | `test_crypto_mac` |
| REQ-SECOC-011 | Secret key resides only in the M0+ image; app uses the MAC oracle only | D6, D7 | `@design-only` + `test_crypto_mac` |
| REQ-SECOC-012 | `CRYPTO_OP_MAC=0x03` generic op: `key_id∥len∥msg ⇒ tag[16]` | D7 | `test_crypto_mac` |
| REQ-SECOC-013 | Telemetry `0x200` (B→A) is authenticated on the same path | D7 | `test_secoc` |

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
*(ADR-0021 D3, D8.)*

### REQ-SECOC-007 — receiver accept rule
Per remote sender the receiver shall keep one persisted `floor` (epoch) and, in RAM, one
`high-water` counter per protected ID. After a **successful** MAC check, for a frame
`(epoch, ctr)` on `id`: reject if `epoch < floor`; if `epoch > current_epoch`, adopt
(`current_epoch=epoch`, reset all high-water) ; within `current_epoch` accept iff `ctr >
high-water[id]`, then set `high-water[id]=ctr`. The MAC check shall occur **before** any freshness
state is updated, so a forged frame cannot advance high-water. *(ADR-0021 D4.)*

### REQ-SECOC-008 — receiver-reset resync
On boot a receiver shall raise `floor = persisted_epoch + 1` (rejecting the entire prior epoch)
and shall send an **authenticated** `FRESHNESS_SYNC` on CAN ID **`0x2F0`** carrying its new floor.
On receipt the peer shall set its sender `epoch = max(current, requested_floor)`. `FRESHNESS_SYNC`
shall itself be a protected frame (REQ-SECOC-001..004) so a replayed or forged sync is rejected.
*(ADR-0021 D5; ADR-0002 SecOC-management sub-band extension.)*

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
