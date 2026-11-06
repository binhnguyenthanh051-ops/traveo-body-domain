# F-017 and F-018 on silicon — 2026-11-06

The two W45 findings with firmware changes, checked on both kits before the vacation:
**F-018** (`LOG_EVT_CRYPTO_ERROR` is now emitted on every failed crypto op) and **F-017** (Node B's
once-a-second CAN status records are now behind `CAN_STATUS_BRINGUP`, off by default). Session
run by Claude Code under the bench rule (`docs/workflow.md`, `bench-test` skill). The owner had
both kits plugged in, and nothing else was physical.

**Images:** built and flashed by the three `zz_build_*` scripts from branch
`bench/2026-11-05-closeout` at the commit that is `32da54d` on `main` (firmware = `main` + F-018 `b17c7f5` + F-017 `0bcb8ec`; hashes as merged).
Node A: `fbl_combined` (built as `bootloader/build/app_combined.hex`) + the signed
`app_stamped.hex` (ECDSA P-256, key_id 1); Node B: `app_combined.hex`. The Seam 6 fault images
are `proj_cm0p` only (`program cm0p DEFINES=FBL_M4_SEAM6_…`), and the normal CM0+ image was
flashed back last. Every flash ended `** Verified OK **` on the `bench.env` probe for its node
(`1F110D8900212400` = A, `1C09043C03272400` = B). Every hex lies in code flash only
(`0x1000_xxxx`, `0x1002_xxxx`, `0x1004_xxxx`, `0x1008_xxxx`); none touches supervisory flash.

**Resets:** each `program` ends in its own reset; the others came from `tools/bench/reset_node.sh`
(OpenOCD). **None is a reset-button boot.** Node B logs `XRES | SOFT` (`0x10010`) for them.

**Captures (this folder):**

| File | What |
|---|---|
| `nodeA.bin`, `nodeB.bin` | raw captures, COM4 / COM6, started before the first flash |
| `nodeA.txt`, `nodeB.txt` | folded transcripts (`logview --file`) |
| `summary.txt` | `host_tools/bench_summary.py` over both |
| `flash_fbl.log`, `flash_app.log`, `flash_nodeB.log` | normal images |
| `flash_cm0p_seam6_dead.log`, `flash_cm0p_seam6_garbage.log`, `flash_cm0p_normal.log` | Node A CM0+ fault images, then the normal one back |

**Sequence (local time):**

| # | Time | Action | Node A boots | Node B boots |
|---|---|---|---|---|
| 1 | 08:12–08:13 | flash FBL, app (A), app (B) | fbl + app (jump) | 1 |
| 2 | 08:14:02–18 | reset A, B, A, B | 2 × (fbl + app, jump) | 2 |
| 3 | 08:15:56 | flash `proj_cm0p` with `FBL_M4_SEAM6_DEAD`, then reset A at 08:16:01 | 2 × fbl, **no jump** | — |
| 4 | 08:16:51 | flash `proj_cm0p` with `FBL_M4_SEAM6_GARBAGE` | fbl + app (**jump**, see F-020) | — |
| 5 | 08:17:33 | flash normal `proj_cm0p`, then reset A at 08:17:39 | 2 × (fbl + app, jump) | — |

`summary.txt`: Node A 16 boots (9 fbl, 7 app, 7 jumps), 1 banner per boot, **0 records lost**;
Node B 3 boots, 1 banner each, 0 lost. No boot appears on the other node's port.

## F-018 — `LOG_EVT_CRYPTO_ERROR` on a crypto failure: **PASS**

**Dead CM0+ (step 3).** Both FBL boots under `FBL_M4_SEAM6_DEAD` show the event and stay in the
bootloader (`nodeA.txt`, the two `fbl` blocks after the 08:15 flash):

```
[       0 app] LOG_EVT_BOOT reset_reason=0x0
[    3000 app] LOG_EVT_CRYPTO_ERROR op=2 reason=0x2
[    3000 app] LOG_EVT_APP_REJECT fail_addr=0x10040000 reason=0x1
```

`op=2` is `CRYPTO_OP_VERIFY_IMAGE` and `reason=2` is `CRYPTO_FAIL_IPC_TIMEOUT`. It comes at
3000 ms: the boot verify starts after the knock window, then waits out the 1 s
`CRYPTO_VERIFY_TIMEOUT_MS`. Exactly one event per failed verify, then the reject, and no
`APP_JUMP`. That is what `test_crypto_error` pins on the host.

**Bad reply on the MAC path (step 4).** With `FBL_M4_SEAM6_GARBAGE`, the Node A **app** (which
should not have run, F-020) got a corrupted reply to every MAC request. That gave a second,
unplanned view of the event on the app's SecOC path. From that boot until the normal CM0+ came
back:

| Event | Count |
|---|---|
| `LOG_EVT_CRYPTO_ERROR op=3 reason=0x4` (MAC, bad reply) | 6,843 |
| `LOG_EVT_CRYPTO_ERROR op=3 reason=0x5` (MAC, refused) | 74 |
| `LOG_EVT_SECOC_REJECT_MAC can_id=0x200` (reason 3, oracle error) | 1,973 |

Every received `0x200` was rejected as an oracle error, not a forgery, and alongside each one is
the crypto event that explains it (the RX overlap ADR-0023 D12 describes). The rest are the send
path: frames the app couldn't MAC and so didn't send, where `CRYPTO_ERROR` is the only evidence.
The 74 `reason=5` are a misclassification with the same root cause as F-020: the hook flips the op
byte, so about 1 tag in 85 happens to start with a byte ≤ 2, and `crypto_parse_verdict`, which
doesn't check the op, reads it as a verdict.

**No false positives.** In the normal phases (steps 1–2 and 5), Node A logged **0**
`CRYPTO_ERROR` and Node B logged **0** in the whole capture, while the MAC path ran
continuously: 12,289 `SECOC_ACCEPT` on `0x200` at Node A, 12,297 on `0x121` at Node B.

Not exercised on silicon: a MAC failure on **Node B**. No bench switch makes Node B's CM0+ fail,
and halting it with the debugger is outside the bench rule. Node B runs the same
`crypto_service.c`; its failure paths are host-tested (`test_crypto_error`).

## F-017 — no CAN status records on Node B's normal image: **PASS**

Before the flash, the old image logged `DBG_U32` tags `0x3510`/`0x3511`/`0x3512` (PSR/ECR/CCCR)
every second: 132 of them in the first ~44 s of `nodeB.bin`. From the first boot of the new image
on, there are **none** across 3 boots and about 6 minutes. On the build side, the tag constant
`0x3510` appears in the `CAN_STATUS_BRINGUP=1` CM7 image and not in the normal image, which was
built last.

What remains is two one-shot `DBG_U32` records per boot (tags `0x3500` = 40000000 and `0x3520` =
0, at 0 ms and 25 ms). They are not F-017's: they don't repeat, and they cost 2 lines per boot. See
below.

## Seen on the way

- **New, F-020: `FBL_M4_SEAM6_GARBAGE` does not make the FBL refuse the app.** The hook XORs
  byte 0 of the encoded reply (`main_cm0p.c:97`), and byte 0 is the **op code**, not the verdict.
  `crypto_msg_decode` doesn't check the op, and neither does `crypto_parse_verdict`, so the
  uncorrupted `VALID` verdict is accepted and the FBL jumps (step 4: `APP_JUMP 0x10040000` at
  2453 ms, the normal timing). This contradicts `docs/briefs/M4-bringup_log.md` §10, which records
  the garbage case as "rejected … immediately" on 2026-07-14. The hook and the verify-path decode
  are unchanged since `51dfe0c` (the Seam 6 commit), and M4 had no log channel, so that
  observation can't be re-checked. Not caused by F-018: `b17c7f5` only adds reports. Registered,
  triage at the retro.
- Node B, one `SECOC_REJECT_FRESHNESS can_id=0x121 rx_ctr=143`, right after the second Node B reset
  in step 2: an old in-flight frame refused across the reset. This is the designed behaviour
  (REQ-SECOC-007), seen before (`docs/bench/2026-10-24/g2_g3_reset_replay.md`).
- Node B's two boot-time `DBG_U32` (`0x3500`, `0x3520`): not registered. If they should go
  behind a switch too, raise it at the retro.
