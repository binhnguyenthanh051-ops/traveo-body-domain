# G1 — Power cycles: the epoch advances and never repeats, 2026-10-24

M5B bench, checklist block "G1" (`docs/project/weeks/2026-W43.md`). Evidence for **REQ-SECOC-006** on silicon:
the sender epoch is persisted in the ADR-0024 work-flash store, so a real power loss (USB pulled, not the reset
button) never reuses an epoch. This also covers ADR-0024 **V5**.

**Capture:** `bus_g1.txt` (can_sniff, 148 s, one session). No COM logs: logview dies when USB is pulled.
The bus carries each node's sender epoch in every secured frame, so it's enough on its own.

## Result: 5 power cycles, every epoch strictly +1 ✅

| Cycle | Power off (bus silent) | A epoch (`0x121`) | B epoch (`0x200`) | B's `0x2F0` floor | Notes |
|---|---|---|---|---|---|
| — | before | 17 | 19 | — | |
| 1 | A 2.30 → 16.98 s, B 2.30 → 12.51 s | **18** | **20** | 0x12 = 18 | A came back 4.5 s after B |
| 2 | 32.43 → 44.31 s (both) | **19** | **21** | 0x13 = 19 | |
| 3 | 53.54 → 64.61 s (both) | **20** | **22** | 0x14 = 20 | |
| 4 | A 76.44 → 89.65 s, B 80.46 → 89.65 s | **21** | **23** | 0x15 = 21 | A unplugged 4 s before B |
| 5 | A 120.99 → 145.08 s, B 120.99 → 140.39 s | **22** | **24** | 0x16 = 22 | **SW3 held** through the power loss (`TELEMETRY 00 00 01` up to 120.990 s) |

- Both senders go up by exactly one per power cycle. **No epoch ever repeats or goes backwards** (A 17 → 22,
  B 19 → 24). After each cycle, each ID restarts at counter 1.
- Node B's floor (the `0x2F0` payload, u16 LE) is always Node A's previous epoch + 1, which is exactly Node A's
  new sender epoch. So Node B accepts Node A from its first command, even in cycles 1 and 5 where Node A was still
  in its FBL and missed the `0x2F0`. That's the sender self-healing described in ADR-0021 D5.
- **F-014 wasn't triggered.** Both nodes were power-cycled each time, so both senders came up at persisted + 1 and
  each receiver's floor was met. Node A's `0x121` (sent only after it accepts a `0x200`) resumed immediately after
  every cycle. That confirms the F-014 analysis: only a Node A reboot on its own deadlocks.

## What cycle 5 does and doesn't show

Cycle 5 lost power with SW3 held and traffic flowing (light commands at 100 %). The epochs still only went up.
But **traffic doesn't cause store commits**: a commit happens only at boot (`secoc_tx_boot`), on an epoch
adoption, or on a counter rollover. So this cycle doesn't hit the "power loss *during* a commit" window. The
checklist overstated this. That window is a few ms per boot, too short to hit by hand. It's covered by the host
tests of ADR-0024 D2 (mark written last: a torn commit leaves the old slot valid and wastes at most one epoch).
On silicon it would need a fault-injection hook (power-fail during erase/program), which is out of scope for M5B.

## For the "On silicon" column (Sun 25)

- **REQ-SECOC-006** → this file (5 real power cycles, epochs strictly increasing on both nodes), plus the
  reset-button evidence in `g2_g3_reset_replay.md`.
- **ADR-0024 V5** → closed: power cycles here, the Node B reset + replay in `g2_g3_reset_replay.md`. The
  replayed ID was `0x121`, not the `0x120` in V5's text, because the checklist chose `0x121`. It is the same
  receiver path.
