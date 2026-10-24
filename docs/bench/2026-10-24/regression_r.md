# R — Stages 5–6 regression on the M5B images, 2026-10-24

M5B bench, checklist block "R" (`docs/project/weeks/2026-W43.md`). This checks that the ADR-0024 store and the
F-010 fix didn't break M5's end-to-end SecOC (Stage 5) or its attack refusals (Stage 6). Procedure:
`docs/briefs/M5-bringup-plan.md`. Normal images from `m5b/secoc_improve` at `c2b3a5d`.

**Captures:** `bus_regress.txt` (two can_sniff sessions appended; the timestamps restart at line 5953),
`nodeA_regress.txt/.bin` (COM4), `nodeB_regress.txt/.bin` (COM6, decoded text is UTF-16).

Node A was handled during this block (the user says when in the retro). Its two dropouts below are therefore
bench handling, not unexplained resets.

## R1 — Stage 5.4/5.5: secured traffic and the courtesy light ✅

- `0x200` and `0x121` both carry trailers (`len=16`, epoch/ctr/MAC) all the way through, and `0x2F0` appears
  on each Node B boot.
- SW3 was pressed four times (two in each sniffer session). Each press gives `TELEMETRY 00 00 01` and then
  `LIGHT_CMD 64` about 1 ms later; each release gives `00 00 00` → `LIGHT_CMD 00`.
  - Session 1: 17.789 / 20.582 s and 27.827 / 30.095 s.
  - Session 2: 4.979 / 7.122 s and 19.050 / 20.016 s.
- Zero rejects on either node while both were up in a steady state. The `REJECT_FRESHNESS` run on Node A
  is F-014, below.

## R2 — Stage 6: `secoc_attack all` ✅ (second run)

Node B (COM6, uptime ms), run 2:

| Case | Bus | Node B verdict |
|---|---|---|
| forged | 45.684 s, `0x120 len=1 01` | 5951 `REJECT_MAC 0x120 reason 2` (bad length) |
| forged-mac | 46.185 s, `0x120 01` + random trailer | 6452 `REJECT_MAC 0x120 reason 1` |
| bitflip | genuine `0x121`, one MAC bit flipped | 6954 `REJECT_MAC 0x121 reason 1` |
| replay | genuine `0x121` resent ~1 s later | 8455 `REJECT_FRESHNESS 0x121 rx_ctr=283` |
| crossid | 48.689 s, `0x121` bytes on `0x120` | 8956 `REJECT_MAC 0x120 reason 1` |

**5/5 refused, one REJECT each, and zero `ACCEPT can_id=0x120` in the whole Node B log**, so the door was never
commanded. That matches the W40 Stage 6 result. F-011 stays fixed: each attack frame appears once on the bus, so
Node A doesn't echo it.

**Run 1 (35.9–41.3 s) doesn't count.** Node A was silent at that point, so the cases that need a genuine `0x121`
(bitflip, replay) had nothing to capture, and Node B was reset partway through. Its forged, forged-mac and crossid
verdicts were the same as in run 2.

## Found here: F-014, telemetry deadlock after a Node A reboot

After each Node A reboot, Node A logged `REJECT_FRESHNESS can_id=0x200` on **every** telemetry frame (460 in a
row, uptime 2314–11527 ms) until Node B was reset. Node B's `0x2F0` (floor 15) then let traffic recover within
20 ms.

The cause: Node A is also a receiver. With the persistent store, it boots at floor = persisted + 1 = 17, while
Node B is still sending at epoch 16. This is ADR-0021 D5's receiver-reboot deadlock in the other direction, and D5
only covers Node B as the receiver. M5's RAM store hid it, because a cold receiver accepted any epoch. Recorded as
**F-014** in `docs/project/findings.md`, with three options. It goes to the M5 review record on Sun 25.

The F-014 recovery also shows ADR-0021 D5 working with a no-op adopt. Node A's new sender epoch (15) already
equalled Node B's requested floor (15), so `secoc_rx_sync_adopt` correctly logged no `SECOC_RESYNC`, and
`0x121` simply went on at epoch 15.
