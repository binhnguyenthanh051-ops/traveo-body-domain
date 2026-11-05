# W45 findings close-out on silicon — 2026-11-05

The bench checks left open by the Tue 3 findings sweep: **F-006** (one FBL banner), **F-007**
(Node B clears its reset cause), **F-012** (flash/reset the right kit with both plugged in) and
**F-013** (`program` flashes the signed image). First session run end to end by Claude Code under
the bench rule (`docs/workflow.md`, `bench-session` skill): flash, reset, capture, check. The owner
plugged in the kits; nothing else was physical.

**Images:** built and flashed tonight by the three `zz_build_*` scripts, from a tree whose firmware
sources are identical to `main` at `2a462ee`. Node A: `fbl_combined.hex` + the signed
`app_stamped.hex`; Node B: `app_combined.hex`. Every flash ended `** Verified OK **`. All three images
lie in code flash only (`0x1000_0000`, `0x1002_0000`, `0x1004_0000`, `0x1008_0000`); none touches
supervisory flash (`0x1700_0000`).

**Resets:** two came from `program` (`verify reset`), and the others from `tools/bench/reset_node.sh`
(OpenOCD `init; reset run`). **None is a reset-button (XRES pin) boot**, which matters for F-007 below.

**Captures (this folder):**

| File | What |
|---|---|
| `nodeA.bin` / `.txt` | Node A logview, COM4, the whole session (raw / decoded, ACCEPT folded; `--all` expands) |
| `nodeB.bin` / `.txt` | Node B logview, COM6, the whole session |
| `flash_fbl.log`, `flash_app.log`, `flash_nodeB.log` | the three `program` runs, verbatim |

Both captures were already running before the first flash, so every boot below is recorded.
Totals: Node A 23,137 records, Node B 24,613, **0 lost, 0 SecOC rejects** on either node.

## Sequence

| # | Action | Probe | Node A boots | Node B boots |
|---|---|---|---|---|
| 1 | `bash ./zz_build_gateway_fbl.sh program` | `1F110D8900212400` | +1 | 0 |
| 2 | `bash ./zz_build_gateway_app.sh program` | `1F110D8900212400` | +1 | 0 |
| 3 | `bash ./zz_build_actuator_app.sh program` | `1C09043C03272400` | 0 | +1 |
| 4 | `reset_node.sh B` | `1C09043C03272400` | 0 | +1 |
| 5 | `reset_node.sh B` | `1C09043C03272400` | 0 | +1 |
| 6 | `reset_node.sh A` ×2 | `1F110D8900212400` | +2 | 0 |
| | **Total** | | **4** | **3** |

## F-012 — the right kit, every time: **pass**

Each script printed `==> flashing on probe <serial>` with the `bench.env` serial, and each boot
appeared **only on that node's port**: 4 boots on COM4, 3 on COM6, no crossover. Node B's capture
runs straight through all four Node A flashes and resets with no restart, and the other way round.

## F-013 — `program` flashes the signed image: **pass**

`zz_build_gateway_app.sh program` built, signed (`SIGNED (ECDSA P-256, key_id=1)`), and flashed
`build/app_stamped.hex` with `** Verified OK **`. After the reset the FBL **verified the
signature and jumped**: `LOG_EVT_APP_JUMP app_entry_addr=0x10040000`, then `--- app nobuildid ---`
and telemetry accepted at 36 ms. A rejected image would have left Node A in the FBL with no jump.

## F-006 — one FBL banner per boot: **pass**

All 4 Node A boots read the same way:

```
--- fbl nobuildid ---
[       0 app] LOG_EVT_BOOT reset_reason=0x0
[    2466 app] LOG_EVT_APP_JUMP app_entry_addr=0x10040000

--- app nobuildid ---
[       0 app] LOG_EVT_BOOT reset_reason=0x0
```

**One** `fbl` banner per boot (4 in 4), where `docs/bench/2026-11-01/` showed two (3 in 3). The
FBL records still render as `app`, as designed (the rejected half of F-006).

## F-007 — Node B clears its reset cause: **pass**, for debug resets

| Boot | `reset_reason` | Decoded (TRM Table 19-2) |
|---|---|---|
| 1, after flash 3 (first boot of the new image) | `0x40010010` | PORVDDD [30] · XRES [16] · SOFT [4] |
| 2, after reset 4 | `0x10010` | XRES [16] · SOFT [4] |
| 3, after reset 5 | `0x10010` | XRES [16] · SOFT [4] |

- **Boot 1 still carries the power-on bit, as expected.** Those bits built up under the *old*
  image, which never cleared them. The new image logged them and then cleared them.
- **Boots 2 and 3 have no PORVDDD, and they are identical.** Before the fix, every boot repeated
  the union since power-on. TRM §19.2: the bits "remain set until cleared by the firmware or a
  POR reset", and the fix is that clear.
- **`0x10010` is the signature of one OpenOCD reset, not two separate resets.** XRES and SOFT
  both appear, but only one boot is logged per reset. So the debugger produced both inside one
  session (a pin reset at connect plus a SYSRESETREQ in `reset run`) while the core was held.
  The same value on boots 2 and 3 shows the bits don't build up.
- **Not shown:** a reset-button boot. A press of the kit's reset button (the XRES pin) is expected
  to log `0x10000` alone. One press would confirm it, but the fix doesn't depend on the reset type.

## Seen on the way

- **F-016 confirmed:** all 8 Node A `LOG_EVT_BOOT` records read `reset_reason=0x0` (FBL and app).
- **F-014 still holds:** after each of the 4 Node A boots, `0x200` telemetry was accepted from
  36 ms on, with 0 rejects.
- **New, F-017:** Node B's normal image logs `LOG_EVT_DBG_U32` tags `0x3510/11/12` (CAN PSR, ECR,
  CCCR) once a second, from `can_report_status()` in `proj_cm7/src/can_task.c`. That's 1,476 of
  the 1,694 lines in `nodeB.txt`. It's a bring-up aid left on outside `CAN_LOOPBACK_TEST`.
- Both KitProg3s report outdated firmware (`fw-loader`: bridging over HID). Not updated; out of
  scope under the bench rule.
