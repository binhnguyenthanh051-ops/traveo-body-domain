---
name: bench-session
description: Run a bench session on the two TRAVEO T2G kits end to end: flash Node A (FBL + signed app) and/or Node B with the zz_build scripts, reset over OpenOCD, capture both log ports, summarise, write the record in docs/bench/<date>/, and update docs/project/findings.md. Use when the owner asks to flash, run a bench check, verify a fix on silicon, or close a finding that waits on the bench.
---

# Bench session (flash → reset → capture → check → record)

The owner allowed Claude to flash and reset the kits on 2026-11-05, within the **bench rule** in
`docs/workflow.md`. Read that section first; it overrides anything below.

## The rule, short

- **Allowed:** `bash ./zz_build_*.sh program`, `bash ./tools/bench/reset_node.sh A|B`, logview captures.
- **Never:** `erase`, writes to supervisory flash, eFuse or lifecycle, `fw-loader --update-kp3`, or a
  probe that isn't pinned in `bench.env`.
- **A failed flash or verify stops the session.** Report the log; don't retry in a loop.
- **Physical actions are the owner's** (plugging in, power, jumpers, buttons, CAN wiring). Ask, then wait.

Bench facts (COM ports, probe serials, the flash commands) live in `docs/hardware/kits.md` →
"The bench" + "Flashing". Don't duplicate them here; read them.

## Shells: the one thing that bites

- Run the `zz_build_*` scripts and `reset_node.sh` through the **PowerShell tool**, as
  `bash ./…`. PowerShell's `bash` is the ModusToolbox (Cygwin) one, which finds make, GCC and Python.
- Don't run them from the Bash tool's plain `bash` (`/c/msys64/usr/bin/bash`): it drops environment
  variables. `"$BASH" ./…` works there if you must.
- PowerShell 5.1 mangles `"` inside native-command arguments. Put anything non-trivial in a script
  file, not an inline `bash -c "…"`.
- Run logview captures from the Bash tool with `run_in_background`, and stop them with TaskStop.

## Procedure

1. **Preconditions.**
   - Check that `bench.env` and `ec_p256_dev_private.pem` exist at the repo root.
   - Check that both kits are listed:
     `C:\Infineon\Tools\ModusToolboxProgtools-1.8\fw-loader\bin\fw-loader.exe --device-list`.
     If none are listed, ask the owner to plug them in, and stop.
   - Check the COM-port ↔ serial mapping against `kits.md` (each KitProg3 USB-UART's
     `DEVPKEY_Device_Parent` holds the probe serial).
   - Check which tree you build from: firmware sources vs `main`
     (`git diff --quiet main HEAD -- node_a_gateway node_b_actuator shared`).
2. **Folder:** `mkdir -p docs/bench/$(date +%F)`. Captures always go in the folder for the day of
   the bench, never the repo root.
3. **Start both captures first**, so every boot is recorded:
   `python -u host_tools/logview/logview.py --port COM4 --raw docs/bench/<date>/nodeA.bin --no-colour > /dev/null 2>&1`
   (and COM6 → `nodeB.bin`), both in the background. Garbage in the first bytes is logview attaching
   mid-stream and resyncing. That's normal.
4. **Flash** what the session needs, in this order: FBL, then app, then Node B. Each one:
   `$out = bash ./zz_build_….sh program 2>&1`, saved to `docs/bench/<date>/flash_<x>.log`.
   - **Pass:** `==> flashing on probe <the bench.env serial>` and `** Verified OK **`.
   - **Stop:** a `WARNING: … unset` line, a verify failure, or a non-zero exit.
   - Before trusting a new hex, check it is code flash only (`0x10xx_xxxx`; supervisory flash is
     `0x1700_0000`).
5. **Reset** as the checks require: `bash ./tools/bench/reset_node.sh A` (or `B`). Wait about 4 s
   between resets. This is a **debug** reset: Node B logs `XRES | SOFT` (`0x10010`), not a button boot.
   Say so in the record.
6. **Stop the captures** (TaskStop), then:
   - `python host_tools/bench_summary.py docs/bench/<date>/nodeA.bin docs/bench/<date>/nodeB.bin`
     for the numbers: boots per image, banners per boot, reset causes decoded, jumps, accepts and
     rejects per CAN ID, lost records.
   - `logview.py --file X.bin --no-colour > X.txt` for the folded transcripts that get committed.
     Don't commit `--all` live transcripts; the `.bin` holds every record.
7. **Check boot attribution:** each boot must appear only on its own node's port (no crossover);
   that's F-012's proof. Node A boots come as pairs: fbl (+ `APP_JUMP 0x10040000`), then app.
   A missing jump means the FBL rejected the app.
8. **Write `docs/bench/<date>/<topic>.md`**, shaped like `docs/bench/2026-11-05/findings_closeout.md`:
   - images and where they were built from
   - how each reset was made
   - a captures table
   - a sequence table (action, probe, boots per node)
   - one section per check, with pass/fail and the evidence lines
   - "Seen on the way" for anything new. Every claim must trace to a file in the folder.
9. **Register** (`docs/project/findings.md`): move what passed to **Resolved**, citing **`main`'s**
   commit hash (PRs are rebase-merged, so branch hashes go stale) and the bench record. Add anything
   new as the next `F-NNN`, **Open**, with a date or "triage at the retro".
10. **Git:** branch off `main`, commit the bench folder + the register, and push. `gh` is not signed
    in here, so give the owner the `pull/new/<branch>` link (and put the PR text on the clipboard
    with `Set-Clipboard` if asked).

## Decoding `reset_reason`

`bench_summary.py` does this for you. `RES_CAUSE` bits (TRM Table 19-2): [30] PORVDDD, [16] XRES,
[4] SOFT (SYSRESETREQ), [3] TC_DBGRESET, [0] WDT, [5..8] MCWDT0..3, [17..26] BOD/OVD/OCD/PMIC.
The bits stay set until firmware clears them or a POR happens (TRM §19.2). Node B clears them after
logging them (F-007). The FBL logs `fbl_reset_cause_t`, not bits, and Node A's app always sees 0
(F-016).
