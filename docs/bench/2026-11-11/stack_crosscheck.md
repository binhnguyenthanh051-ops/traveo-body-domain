# Bench 2026-11-11: static stack bound vs measured high-water (F-021, ADR-0025 D5/D6)

Run by Claude under the bench rule (`docs/workflow.md`), following the `bench-test` skill. The owner
plugged in both kits; nothing else was physical.

**Question.** For every task `stack_bound` analyses, is the static worst case at least what the task
really used on silicon? If not, the analysis missed a path and F-021's sizes can't be trusted.

**Answer.** Yes, on both nodes, for all 7 analysed tasks. F-021 is resolved on silicon. One new
finding on the way: a sequence-number race in `log_evt` (F-022).

## Images

| Node | Image | Built from | Stack gate (D6) |
|---|---|---|---|
| A | `node_a_gateway/app` → `app_stamped.hex`, signed ECDSA P-256 key_id 1 | branch `fix/f021-can-stack` at `b22b611` (firmware differs from `main`: F-021 + D5) | pass: `gate_nodeA_app.json` (ELF sha256 `13631e8ab0e3`) |
| B | `node_b_actuator` → `app_combined.hex` (CM0+ + CM7) | same | pass: `gate_nodeB_cm7.json` (ELF sha256 `f55ed5eab3fa`) |

Both images went through the new gate in `zz_build_*_app.sh` before flashing. `stack_bound` at
`894e799` (0.2.0). The FBL was not reflashed. Node B's hex is code flash only (`0x1000_0000` region),
checked before trusting it.

## Captures

| File | What |
|---|---|
| `nodeA.bin`, `nodeA.txt` | COM4 (Node A, probe `1F110D8900212400`), raw + folded transcript |
| `nodeB.bin`, `nodeB.txt` | COM6 (Node B, probe `1C09043C03272400`), raw + folded transcript |
| `flash_nodeA_app.log`, `flash_nodeB.log` | the `program` runs, gate output included |
| `gate_node*.json/.html` | the stack_bound reports of the flashed images (the gate's own output) |
| `summary.txt` | `bench_summary.py … --stack-report` output |
| `stack_bound_node*.{txt,json,html}` | the F-021 analysis from earlier today (before D5) |

## Sequence

| Step | Action | Probe | Boots seen |
|---|---|---|---|
| 1 | captures started on COM4 and COM6 | — | — |
| 2 | `bash ./zz_build_gateway_app.sh program`: gate pass, sign, `Verified OK` | `1F110D…` | A: fbl + app (jump) |
| 3 | `bash ./zz_build_actuator_app.sh program`: build, gate pass, `qprogram`, `Verified OK` | `1C0904…` | B: 1 |
| 4 | `bash ./tools/bench/reset_node.sh A`, 4 s, then `… B` (21:03:44) | each | A: fbl + app (jump); B: 1 |
| 5 | ran about 65 s, captures stopped | — | — |

Resets are **debug** resets (OpenOCD), not the button: Node B logs `XRES | SOFT` (`0x10010`), and Node A
logs `0x0` (F-016). Every boot appears only on its own node's port (`summary.txt`: A 4 boots / 2 jumps,
B 2 boots).

## Check 1: static worst case ≥ measured use, per task (REQ-LOG-016) — PASS

From `summary.txt` (`bench_summary.py --stack-report`, measured = configured − 4 × min free words over
the whole capture):

| Node | Task | Measured | Static (stack_bound) | Configured | Static − measured |
|---|---|---|---|---|---|
| A | `can_task` | 1200 B | 1488 B | 2048 B | 288 B |
| A | `app_task` | 964 B | 1256 B | 2048 B | 292 B |
| A | `health_task` | 204 B | 408 B | 512 B | 204 B |
| A | `log_cyclic_task` | 128 B | 308 B | 512 B | 180 B |
| B | `can_task` | 1144 B | 1472 B | 2048 B | 328 B |
| B | `heartbeat_task` | 200 B | 400 B | 512 B | 200 B |
| B | `log_cyclic_task` | 104 B | 308 B | 512 B | 204 B |

The two `can` tasks used 1200 B and 1144 B of the old 1536 B stacks: 78% and 74% measured on the
normal path alone. The static worst case adds the crypto-error logging branch (F-018), which the
normal run never takes, and that branch is what pushed them past 1536 B. The ~290 B between static and
measured on the SecOC tasks is about that branch (160 B) plus the deepest SecOC path not taken under
steady traffic.

FreeRTOS tasks, measured only (no static bound yet, binary witness needed): Node A idle 118 / timer 99
words free; Node B idle 116 / timer 227 words free.

## Check 2: LOG_EVT_TASK_STACK is emitted as designed (D5) — PASS

40 `APP_ALIVE` ticks on Node A, each followed by 6 `TASK_STACK` records (ids 1 2 3 4 6 7); Node B 5
per tick (ids 1 4 5 6 7). 138 / 100 lines in the transcripts.

## Check 3: the stack gate in the real flashing path (D6) — PASS, after two fixes

The first two `program` runs stopped at the gate with stacks that passed: under the ModusToolbox Cygwin
shell, Windows Python couldn't open the `/cygdrive` JSON path, then printed `pass\r`. The gate failed
closed both times; fixed in `b22b611` (cygpath + strip CR). An earlier run also hit CRLF line endings in
the scripts, from a Windows-side edit; restored from the index (`.gitattributes` pins `*.sh` to LF).

## Seen on the way

- **F-022 (new): `log_evt` sequence-number race.** Node A reports `lost: 64`, but no record is missing:
  at t = 65.000 s the `can` task's `SECOC_ACCEPT` and the health task's first `TASK_STACK` both carry
  **seq 38**, and seq 39 never appears. The decoder reads 38 → 38 as 63 lost (6-bit wrap) and 38 → 40
  as 1 lost. `log_evt()` reads `s_seq` before the masked enqueue and increments it after
  (`shared/log/src/log.c:207`, `:226-229`), so a preempting producer gets the same number. Older than
  today; D5's 7-record burst next to ~48 accepts/s made it show (1 of 40 ticks on A, 0 on B).
- Node B: one `SECOC_REJECT_FRESHNESS` on `0x121` after its reset, as on 2026-11-06 (same count, same
  ID): the receiver-reset resync path, not new.
- Traffic: `0x200` ×8939 accepted on A, `0x121` ×8946 on B, 0 MAC rejects.
