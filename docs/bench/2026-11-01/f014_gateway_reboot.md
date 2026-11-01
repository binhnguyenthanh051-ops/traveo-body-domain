# F-014 — Node A reboots on its own, telemetry recovers, 2026-11-01

Bench for **F-014** (ADR-0021 addendum "D5 in both directions"): Node A sends an authenticated
`GW_FRESHNESS_SYNC` on `0x1F0` at boot, and Node B adopts the floor into its telemetry epoch.
Evidence for **REQ-SECOC-008** in the A-receiver direction, plus a short M5B regression.
Checklist: `docs/project/weeks/2026-W44.md`, blocks F and R.

**Images:** built 2026-10-30 22:07 from `m5b/close-out` at `b69ed7b`. Node A: `fbl_combined.hex`
(unchanged since M5B) + the signed `app_stamped.hex`; Node B: `app_combined.hex`. The banners say
`nobuildid`, so the build time can't be read from them. The new images are proven by behaviour
instead: `0x1F0` frames exist only in the new Node A, and `RESYNC can_id=0x1F0` only in the new Node B.

**Captures (this folder):**

| File | What |
|---|---|
| `bus.txt` | can_sniff, ~268 s: 13,292 × `0x200`, 12,750 × `0x121`, 4 × `0x1F0`, 3 × `0x2F0`, 3 × `0x120` (attacks) |
| `nodeA.bin` / `.txt` | Node A logview, COM4 (raw / decoded) |
| `nodeB.bin` / `.txt` | Node B logview, COM6 (raw / decoded) |
| `old_0x200.json` | genuine telemetry saved before an A reset (F2) |
| `old_0x1F0.json` | Node A's boot sync from reset 2, replayed after reset 3 (F5) |
| `old_0x121.json` | genuine light command saved before a Node B reset (R2) |

## F — Node A reset alone (3 times)

| Reset | `0x1F0` on the bus (A epoch) | Node B | `0x200` back | Node A rejects on `0x200` |
|---|---|---|---|---|
| 1 (129.8 s) | epoch 26, ctr 1 | `RESYNC can_id=0x1F0 new_epoch=29` | epoch 29 ctr 1, 28 ms after the sync; accepted 36 ms after A's app start | **0** |
| 2 (147.2 s) | epoch 27, ctr 1 | `RESYNC … new_epoch=30` | epoch 30 ctr 1, +28 ms; accepted at 35 ms | **0** |
| 3 (191.4 s) | epoch 28, ctr 1 | `RESYNC … new_epoch=31` | epoch 31 ctr 1, +29 ms; accepted at 35 ms | **1** (one epoch-30 frame in flight at 1 ms) |

**Sat 24, before the fix:** 460 `REJECT_FRESHNESS` in a row after one A reset, with no recovery
until Node B rebooted. **Now:** 0, 0 and 1. Node B was not touched, and its uptime runs on
through all three resets.

| Check | Expected | Seen | Verdict |
|---|---|---|---|
| F1 link healthy | `0x200` accepted on A | B epoch 28, A epoch 25, accepted continuously | ✅ |
| F3 A reset alone, telemetry recovers | sync on `0x1F0`, B adopts, few rejects | table above | ✅ |
| F4 old telemetry after the reset (REQ-SECOC-007, B→A) | `REJECT_FRESHNESS` on A | saved `0x200` epoch 28 ctr 5554, sent at 171.7 s → **`SECOC_REJECT_FRESHNESS can_id=0x200`** | ✅ |
| F5 old sync replayed after the next A boot | refused, B's epoch doesn't move | `0x1F0` epoch 27 ctr 1 (from reset 2), sent at 194.4 s after reset 3 → **`SECOC_REJECT_FRESHNESS can_id=0x1F0 rx_ctr=1`** on B, no RESYNC, `0x200` stays at epoch 31 | ✅ |
| F6 three A resets | same every time | 3 of 3 | ✅ |

## R — M5B regression

| Check | Expected | Seen | Verdict |
|---|---|---|---|
| R1 `secoc_attack all` | 5/5 refused, no `0x120` accept | B: `REJECT_MAC 0x120 reason 2`, `REJECT_MAC 0x120 reason 1`, `REJECT_MAC 0x121 reason 1`, `REJECT_FRESHNESS 0x121`, `REJECT_MAC 0x120 reason 1`; no ACCEPT on `0x120` | ✅ |
| R2 Node B reset → A resyncs → old `0x121` refused | `SECOC_RESYNC 0x2F0` on A, `REJECT_FRESHNESS` on B | 3 B resets (two 1 s apart, then one 30 s later; all owner presses): A `RESYNC can_id=0x2F0 new_epoch=29/30/31`, `0x121` at ctr 1 of the new epoch ~21 ms after each `0x2F0`. Saved `0x121` epoch 30 ctr 965 → **`REJECT_FRESHNESS can_id=0x121 rx_ctr=965`** on B, 2 s after its boot | ✅ |
| R3 power cycle of both | both epochs +1 | done after the logs were stopped, **not captured** | — not counted |

R3 has no evidence from this session. REQ-SECOC-006 keeps its 2026-10-24 evidence
(`docs/bench/2026-10-24/epochs_power_cycle.md`, 5 power cycles).

## T — triage, read from the same captures

T was not run as its own block, but the F and R resets are exactly the boots it asks for.

| Check | Criterion (checklist) | Seen | Verdict |
|---|---|---|---|
| T1 F-008: reset-button boot of Node B | one banner + one `LOG_EVT_BOOT` → it was the programmer → reject | 3 presses (owner-confirmed), each gives **one** `--- actuator nobuildid ---` + **one** `LOG_EVT_BOOT reset_reason=0x40010000`. The two boots 1 s apart were two presses. | **reject F-008** |
| T2 F-006: reset-button boot of Node A | count `--- fbl … ---` banners before the FBL `LOG_EVT_BOOT`, note the source tag | 3 of 3 resets: **two** `--- fbl nobuildid ---` banners, then one `LOG_EVT_BOOT` and `LOG_EVT_APP_JUMP` tagged **`app`**, not `fbl` | **reproduced**, stays open (off-bench check of where the FBL prints its banner and how logview picks the tag) |

## Notes

- The epoch numbers line up across the two nodes by coincidence (B's telemetry and A's view of it
  both reached 29–31). They are separate domains.
- The capture was saved to `2026-10-31/` (the folder in the checklist) and moved to this one,
  because the bench ran on 2026-11-01.
