# G2 + G3 — Node B reset: resync, then an old frame is refused, 2026-10-24

M5B bench, checklist block "G2 + G3" (`docs/project/weeks/2026-W43.md`). Evidence for
**REQ-SECOC-007** (replay refused across a receiver reboot, G2) and **REQ-SECOC-008** (resync
from FRESHNESS_SYNC on silicon, G3). Both use the ADR-0024 work-flash freshness store.

**Images:** built from `m5b/secoc_improve` at `c2b3a5d` (includes F-010 fix `1f40dfc`), at 21:55–21:57.
They are normal images: `g_kat_result` is absent from every ELF. Node A: `fbl_combined.hex` + the signed
`app_stamped.hex`. Node B: `app_combined.hex`.

**Captures (this folder):**

| File | What |
|---|---|
| `bus_round2.txt` | can_sniff, 95 s, 18,077 frames (9037 × `0x121`, 9036 × `0x200`, 4 × `0x2F0`) |
| `nodeA_round2.txt` / `.bin` | Node A logview, COM4 (decoded / raw) |
| `nodeB_round2.txt` / `.bin` | Node B logview, COM6 (decoded text is UTF-16 LE, from PowerShell `tee`) |
| `old_0x121_round2.json` | the saved genuine frame (`secoc_attack replay --save`) |
| `bus_step4.txt` | the first attempt (no COM logs), see "Round 1" below |

## Result

| Check | Expected | Seen | Verdict |
|---|---|---|---|
| Node B boot sends its floor | `0x2F0` floor = persisted + 1 | 4 resets → floors **5, 6, 7, 8** | ✅ |
| Node A completes the resync (G3) | `SECOC_RESYNC`, `0x121` moves to the new epoch | `SECOC_RESYNC new_epoch=5/6/7/8`, `0x121` at counter 1 of the new epoch, **20 ms** after each `0x2F0` | ✅ |
| Node A is not rebooted | no boot event on COM4 | none; uptime continuous (733 s → 792 s) | ✅ |
| Old frame refused after a reset (G2) | `REJECT_FRESHNESS`, no actuation | **`SECOC_REJECT_FRESHNESS can_id=0x121 rx_ctr=1`** | ✅ |
| No collateral rejects | genuine traffic unaffected | Node B: 1 reject in total (the replay); Node A: 0 rejects | ✅ |

## G3 — resync on each Node B reset

Only Node B's reset button was pressed. Times are can_sniff seconds; Node A times are its uptime in ms.

| # | Node B silent (`0x200`) | `0x2F0` floor | Node A: accept `0x2F0` → `SECOC_RESYNC` | First `0x200` / `0x121` at the new epoch |
|---|---|---|---|---|
| 1 | 15.893 → 16.396 (0.50 s) | 5 @ 16.376 | 733967 → 733979 | 16.396 / 16.397, ctr 1 |
| 2 | 22.280 → 22.748 (0.47 s) | 6 @ 22.728 | 740339 → 740351 | 22.748 / 22.749, ctr 1 |
| 3 | 30.691 → 31.000 (0.31 s) | 7 @ 30.980 | 748614 → 748627 | 31.000 / 31.001, ctr 1 |
| 4 | 32.264 → 32.615 (0.35 s) | 8 @ 32.595 | 750233 → 750246 | 32.615 / 32.616, ctr 1 |

- Node B logged `LOG_EVT_BOOT reset_reason=0x40010000` on every reset. That is `CY_SYSLIB_RESET_XRES` (0x10000,
  the button) plus `CY_SYSLIB_RESET_PORVDDD` (0x40000000), which is most likely left over from the first power-on
  because nothing clears it. Its first log after boot is `SECOC_ACCEPT can_id=0x121 freshness_ctr=1` at 21 ms.
- **Zero `REJECT_FRESHNESS` during the resyncs.** The checklist expected "a few" on `0x121`, because Node A would
  still be at the old epoch. That didn't happen: Node B's `0x2F0` goes out before its first telemetry. Node A only
  sends `0x121` in reply to a `0x200` it has accepted, so it adopts the floor before it sends its next command.
  No old-epoch command reaches Node B after the reset. This is the intended ADR-0021 D5 behaviour, not missing data.
- Node A takes about 12 ms from accepting the `0x2F0` to logging `SECOC_RESYNC`. That window includes the
  ADR-0024 commits for the receiver epoch and the adopted sender floor. This is **not** a commit-latency
  measurement (ADR-0024 V4), because log timestamps are at 1 ms resolution and it includes the MAC verify. Use it
  only as an order-of-magnitude bound.

## G2 — replay of an old genuine frame after a reset

1. `replay --save` (22:19:17) captured a genuine `0x121` at **epoch 7, counter 1**: the first frame after
   reset #3 (`old_0x121_round2.json`, data `0007000100bc9b78bc4292b227000000`). The plan was epoch 4, but the
   save was still waiting for a frame when reset #3 happened. The test is still valid, because epoch 7 is below
   the floor Node B set after the next reset.
2. Reset #4 → Node B's floor is **8**.
3. `replay --load` at bus t = 52.511 s:

```
52.508  121  LIGHT_CMD 00 | epoch 8 ctr 948 | mac 8cd8a4d4e52940e0   genuine
52.511  121  LIGHT_CMD 00 | epoch 7 ctr   1 | mac bc9b78bc4292b227   <- replayed (MAC is genuine)
52.532  121  LIGHT_CMD 00 | epoch 8 ctr 949 | mac 03a8fd640fd596f2   genuine
```

Node B (COM6, uptime ms):

```
19913  LOG_EVT_SECOC_ACCEPT can_id=0x121 freshness_ctr=948
19916  LOG_EVT_SECOC_REJECT_FRESHNESS can_id=0x121 rx_ctr=1
19937  LOG_EVT_SECOC_ACCEPT can_id=0x121 freshness_ctr=949
```

The frame's MAC is valid, because it was sent by Node A. It is refused only because epoch 7 is below Node B's
persisted floor of 8. The next genuine frame is accepted, so the replay didn't disturb the receiver's state.

**Compared with M5:** with M5's RAM-backed store, a Node B reset cleared the floor, so this same kind of frame was
**accepted** (M5 runbook, Stage 7 note; gap G2 in the W43 plan). With the ADR-0024 work-flash store, it is refused.

**Caveat:** the replayed payload is `LIGHT_CMD 00` (brightness 0), the same as the current state, so "no actuation"
can't be seen on LED4. The evidence is Node B's `REJECT_FRESHNESS` line. A replay that would visibly actuate needs
a saved frame with SW3 held (`LIGHT_CMD 64`). That's optional for the post, and it isn't needed for REQ-SECOC-007.

## Round 1 (`bus_step4.txt`) — why it was redone

The first attempt had only can_sniff running, with no COM logs.

- Reset #1 at t = 12.55 s was a clean resync (floor 3, both IDs at epoch 3, counter 1, 20 ms later).
- In round 1's second reset, Node A stopped sending `0x121` at t = 15.57 s, **0.84 s before** Node B went silent
  (16.41 s). It came back at 19.32 s at epoch 4, counter 1, without ever adopting Node B's floor 4: the `0x2F0`
  arrived while Node A was still booting. Working back from the round-2 log uptimes and file times puts Node A's
  app start at about 22:06:49.8 and Node B's at about 22:06:48.2. That matches Node B's reboot in `bus_step4.txt`
  to within 0.5 s. So **Node A rebooted**, and nobody touched it on purpose. A stall doesn't fit: a hung Node A would
  have answered its queued epoch-3 telemetry when it recovered, and a crypto failure would have left it at epoch 3.
  Its assert and stack-overflow traps also spin forever, and the WDT is disabled, so this was a real reset
  (XRES, power or brown-out, or a software reset).
- The reset reason is unknown because COM4 wasn't logged. Leading guess: Node A's USB cable was nudged while
  reaching for Node B's reset button, or a dip on a shared USB hub. **Watch for it in G1:** if Node A reboots
  without being touched again, the `LOG_EVT_BOOT reset_reason` line will tell. Open a finding only if it happens again.
- Round 1's epoch evidence still holds: it never repeats and only goes up (2 → 3 → 4). Its second reset doesn't
  count as G3 evidence, which is why it was redone as round 2.

## For the "On silicon" column (Sun 25)

- **REQ-SECOC-007** → this file, section G2 (`nodeB_round2.txt` at 19916 ms, `bus_round2.txt` at 52.511 s).
- **REQ-SECOC-008** → this file, section G3 (four resyncs; `nodeA_round2.txt` `SECOC_RESYNC` lines).
- **REQ-SECOC-006** (partial; G1 power cycles still to come) → the epoch is persisted across XRES: the floors
  went 5 → 6 → 7 → 8 and never repeated.
