# Bench 2026-11-12: F-020 (Seam 6 GARBAGE) and F-022 (`log_evt` sequence race) on silicon

Run by Claude under the bench rule (`docs/workflow.md`), following the `bench-test` skill. The owner
plugged in both kits; nothing else was physical.

**Questions.**
1. F-022: with the sequence number taken inside the masked region, does the same load that gave
   `lost: 64` on 2026-11-11 now give `lost: 0`?
2. F-020: with the verdict parser checking the op code, does `FBL_M4_SEAM6_GARBAGE` make the FBL
   refuse the app?

**Answers.** Yes to both. F-022: 0 records lost in 53,884 on Node A and 53,724 on Node B, across 223
health ticks on Node A. F-020: two FBL boots under the fault image, each `CRYPTO_ERROR op=2
reason=0x4` then `APP_REJECT`, no jump. The normal CM0+ was flashed back last and Node A boots again.

## Images

| Node | Image | Built from | Stack gate (ADR-0025 D6) |
|---|---|---|---|
| A | `node_a_gateway/bootloader` → `app_combined.hex` (FBL CM4 + CM0+) | branch `fix/f020-f022-findings` at `9835ace` (firmware = `main` + `82dd786` F-022 + `805afa6` F-020) | not gated (FBL) |
| A | `node_a_gateway/app` → `app_stamped.hex`, signed ECDSA P-256 key_id 1 | same | pass: `gate_nodeA_app.json` (ELF sha256 `60d6eb89202a`) |
| B | `node_b_actuator` → `app_combined.hex` (CM0+ + CM7) | same | pass: `gate_nodeB_cm7.json` (ELF sha256 `7de1298d7ad0`) |
| A | `proj_cm0p` only, `DEFINES=FBL_M4_SEAM6_GARBAGE` → `gateway_fbl_cm0p.hex` | same | fault image, step 6 |
| A | `proj_cm0p` only, no switch → `gateway_fbl_cm0p.hex` | same | the normal CM0+, flashed back in step 8 |

All five flashes printed `==> flashing on probe <the bench.env serial>` and `** Verified OK **`
(`1F110D8900212400` = A, `1C09043C03272400` = B). Every hex is code flash only (`0x1000_xxxx`,
`0x1002_xxxx`, `0x1004_xxxx`, `0x1008_xxxx`); none touches supervisory flash. `stack_bound` at
`v0.2.0`.

**Resets:** each `program` ends in its own reset; the others came from `tools/bench/reset_node.sh`
(OpenOCD). **None is a reset-button boot.** Node B logs `XRES | SOFT` (`0x10010`); Node A logs `0x0`
(F-016).

## Captures

| File | What |
|---|---|
| `nodeA.bin`, `nodeA.txt` | COM4 (Node A), steps 1 to 9: raw + folded transcript |
| `nodeB.bin`, `nodeB.txt` | COM6 (Node B), steps 1 to 9 |
| `nodeA_soak.bin/.txt`, `nodeB_soak.bin/.txt` | step 10: ten more minutes, no flash, no reset |
| `summary_f022.txt` | `bench_summary.py` at the end of step 5, before any fault image |
| `summary.txt` | `bench_summary.py … --stack-report` over steps 1 to 9 |
| `summary_soak.txt` | `bench_summary.py` over step 10 |
| `flash_fbl.log`, `flash_nodeA_app.log`, `flash_nodeB.log` | the normal `program` runs, gate output included |
| `flash_cm0p_seam6_garbage.log`, `flash_cm0p_normal.log` | the fault image, then the normal CM0+ back |
| `gate_node*.json` | the stack_bound reports of the flashed app images |
| `reset_step4.log`, `reset_step6.log`, `reset_step8.log` | OpenOCD output of the resets |

## Sequence

| Step | Time | Action | Boots seen |
|---|---|---|---|
| 1 | before step 2 | captures started on COM4 and COM6 | — |
| 2 | 19:24:39 | `zz_build_gateway_fbl.sh program` | A: fbl + app (jump) |
| 3 | 19:25:01 | `zz_build_gateway_app.sh program`: gate pass, sign, flash | A: fbl + app (jump) |
| 4 | 19:25:39 | `zz_build_actuator_app.sh program`: gate pass, flash | B: 1 |
| 5 | 19:25:45, 19:25:49 | `reset_node.sh A`, 4 s, `reset_node.sh B`; then ran to 19:31 | A: fbl + app (jump); B: 1 |
| 6 | 19:31:34 | `zz_build_gateway_fbl.sh program cm0p DEFINES=FBL_M4_SEAM6_GARBAGE` | A: fbl, **no jump** |
| 7 | 19:31:41 | `reset_node.sh A` | A: fbl, **no jump** |
| 8 | 19:32:26 | `zz_build_gateway_fbl.sh program cm0p` (normal) | A: fbl + app (jump) |
| 9 | 19:32:30 | `reset_node.sh A`; ran about a minute, captures stopped | A: fbl + app (jump) |
| 10 | after 9 | new captures, 10 min, nothing touched | none |

Every boot appears only on its own node's port (`summary.txt`: A 12 boots / 5 jumps, B 2 boots).

## Check 1 (F-022): no false "lost" under the same load — PASS

The load is the one that showed the race on 2026-11-11: about 48 `SECOC_ACCEPT` per second from the
`can` task, and every 5 s the health task's `APP_ALIVE` + 6 `TASK_STACK` burst.

| Capture | Node A records | Node A lost | Node A health ticks | Node B records | Node B lost |
|---|---|---|---|---|---|
| 2026-11-11 (before the fix) | 9,127 | **64** | 40 | 9,096 | 0 |
| steps 1 to 5 (`summary_f022.txt`) | 19,752 | 0 | | 19,696 | 0 |
| steps 1 to 9 (`summary.txt`) | 24,308 | 0 | 102 | 24,271 | 0 |
| step 10 (`summary_soak.txt`) | 29,576 | 0 | 121 | 29,453 | 0 |

0 resyncs and 0 rejects in every capture today.

**How strong this is.** The race needs a health tick to land inside a `can` task `log_evt()`. Before
the fix it hit 1 tick in 40 on Node A (one observation, so the rate is rough). Today: 223 ticks, 0
hits. At 1 in 40, 223 clean ticks by chance is about 0.4%. The bench shows the symptom is gone; the
proof that the mechanism is closed is the host test (`test_preempted_producer_gets_its_own_sequence_number`,
which failed before `82dd786` with the bench's own symptom).

## Check 2 (F-020): Seam 6 GARBAGE makes the FBL refuse — PASS

`nodeA.txt`, the two boots under the fault image (steps 6 and 7), identical:

```
--- fbl nobuildid ---
[       0 app] LOG_EVT_BOOT reset_reason=0x0
[    2449 app] LOG_EVT_CRYPTO_ERROR op=2 reason=0x4
[    2449 app] LOG_EVT_APP_REJECT fail_addr=0x10040000 reason=0x1
```

- `op=2` is `CRYPTO_OP_VERIFY_IMAGE`, `reason=0x4` is `CRYPTO_FAIL_BAD_REPLY`: the reply decoded,
  but its op code did not echo the request, so `crypto_parse_verdict` rejected it.
- No `LOG_EVT_APP_JUMP`, and no app banner until step 8. On 2026-11-06 the same hook gave
  `APP_JUMP 0x10040000` at 2453 ms.
- The rejection comes at 2449 ms, the normal verify time: the CM0+ still hashes and checks the
  image, then corrupts its answer.
- With the normal CM0+ back (steps 8 and 9): `APP_JUMP app_entry_addr=0x10040000` at 2449 ms, twice,
  and the app runs (`0x200` accepted again, 0 rejects).

This is the first log evidence for the "lying CM0+" case. M4's claim (`docs/briefs/M4-bringup_log.md`
§10) predates the log channel and could not be reproduced on 2026-11-06; §10 now carries a dated
correction.

**Not seen on silicon:** the MAC-path half of F-020 (a flipped-op MAC reply logged as `reason=5`
instead of 4). The app never runs under the fault image now, so there is no MAC traffic to
misclassify. It is host-tested only
(`test_mac_wrong_op_reply_is_bad_reply_even_if_it_reads_as_a_verdict`).

## Check 3: the rebuilt images still pass the stack gate and the cross-check — PASS

F-022 moved the lock into `log_evt()`, which changes its frame. From `summary.txt`:

| Node | Task | Measured | Static (stack_bound) | Configured | Static on 2026-11-11 |
|---|---|---|---|---|---|
| A | `can_task` | 1200 B | 1480 B (72%) | 2048 B | 1488 B |
| A | `app_task` | 964 B | 1248 B (61%) | 2048 B | 1256 B |
| A | `health_task` | 204 B | 400 B (78%) | 512 B | 408 B |
| A | `log_cyclic_task` | 136 B | 308 B (60%) | 512 B | 308 B |
| B | `can_task` | 1176 B | 1464 B (71%) | 2048 B | 1472 B |
| B | `heartbeat_task` | 200 B | 392 B (77%) | 512 B | 400 B |
| B | `log_cyclic_task` | 104 B | 308 B (60%) | 512 B | 308 B |

Measured ≤ static on every analysed task, and every static bound is 8 B lower than yesterday on the
paths through `log_evt` (its helper no longer keeps the lock state). Node A's `health_task` moved from
exactly 80% to 78%.

## Seen on the way

- Nothing new for the register.
- Tooling: `timeout … python logview.py` from the Bash tool fails with "pyserial not installed". The
  `timeout` child loses `APPDATA`, the same cause as the stack gate note in `kits.md`. Run the capture
  in the background and stop it instead, as the `bench-test` skill says.
- Traffic today: `0x200` ×52,298 accepted on A, `0x121` ×52,298 on B, 0 MAC rejects, 0 freshness
  rejects.
