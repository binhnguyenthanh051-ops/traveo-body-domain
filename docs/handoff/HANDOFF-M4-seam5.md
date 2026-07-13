# Handoff — TRAVEO T2G portfolio, M4 Seam 5 (TCB isolation)

**Date:** 2026-07-10 · **Branch:** `docs/m3-blog` (note: work is happening here despite the name)
**Repo:** `C:\00_Projects\00_Portfolio\00_TraveoBody\traveo-body-domain`

## How to work in this project (read first)

- **Role/rhythm:** act as an automotive diagnostic-stack / security architect. First deliverable of
  any seam is *design* with tradeoffs + at least one alternative at each boundary (blog/interview
  material). Rhythm: **discuss → agree → ADR → failing host test → implement → on-board bring-up.**
- **Challenge decisions**, don't just agree. Give a recommendation, not an exhaustive survey.
- **Hardware facts** are in `CLAUDE.md` — don't guess them. Board `CYTVII-B-E-1M-SK`, MCU `CYT2B7`,
  asymmetric dual-core **160 MHz CM4F (primary) + 100 MHz CM0+**. Flag anything register-level as
  "verify in TRM".
- **MISRA C:2012** for target/production C (`docs/coding-standard.md`); host tests (`*/tests/`,
  Unity) are exempt. Hardware-independent modules must not include vendor headers (ADR rule 1).
- **User's setup:** runs the board with KitProg3 + OpenOCD debugging **CM4 only** (cannot debug
  CM0+). Firewalled network (raw.githubusercontent.com blocked). MTB 3.8 at
  `C:/Infineon/Tools/ModusToolbox/tools_3.8`. To read CM0+ state, we write debug structs to a fixed
  shared-SRAM address and read them from CM4's OpenOCD with `mdw`.
- **Lifecycle/eFuse:** standing decision to IGNORE lifecycle advancement + eFuse provisioning
  (design-only, nothing burned). NORMAL lifecycle → the DAP is PC0 and bypasses protection units.
- **Memory:** persistent memory dir exists at
  `C:\Users\adminlocal\.claude\projects\C--00-Projects-...-domain\memory\` with `MEMORY.md` index.
  Relevant memories: user knows RTOS concepts but not low-level internals (explain for interviews);
  blog style guide at `docs/blogs/blog-style-guide.md`; "verify preconditions before debugging".

## Build / test commands

- Host tests: `make test` (Unity). Individual: `make test_prot_region`, `make test_boot_secure`, etc.
- **Sandbox gotcha (AI Bash tool only):** `make` recipes run with `TMP=C:\WINDOWS` → gcc "Cannot
  create temporary file" error. The *user's own* terminal builds fine. Workaround when the AI
  compiles directly: set `TMP`/`TEMP` to a writable dir. Example that works:
  ```
  SCR=<writable dir>; TMP="$SCR" TEMP="$SCR" cc -std=c17 -Wall -Wextra -Werror -O1 -g -pipe \
    -Ivendor/unity/src -Ishared/prot/include vendor/unity/src/unity.c \
    shared/prot/src/prot_region.c shared/prot/tests/test_prot_region.c -o build/test_prot_region
  ```
- FBL build/flash: `bash ./zz_build_gateway_fbl.sh`; app: `bash ./zz_build_gateway_app.sh`.
- Flash app over CAN: `python host_tools\uds_flash\uds_flash.py node_a_gateway\app\build\app_stamped.hex`

## Where M4 stands (Seams 0–4 DONE on silicon)

M4 = app secure boot + crypto offload to CM0+. Seams 0–4 are proven on hardware:
- **Seam 4 milestone LANDED:** a `sign_image.py`-signed app, flashed over the M3 UDS bus, boots on a
  real CM0+-computed **SHA-256 + ECDSA-P256** verdict; the app runs (jumps ok).
- Crypto stack: layered transport (ipc_mailbox) ← framing (crypto_msg) ← dispatch ← handlers
  (`crypto_ops_cm0p.c`). CM0+ hashes real flash (coarse RPC, ADR-0017 D1). Byte-order fix (S3-2):
  Crypto VU is little-endian for point coords + sig scalars, hash stays big-endian.
- Trailer geometry (SHA256 mode): `hash[32] + sig[64] r||s + key_id[4]` = 100 B past `image_len`.

### The bug fixed right before Seam 5 (context for the "verify preconditions" lesson)
`check-image` returned NRC 0x72 on a correctly-signed app. Root cause was **`uds_flash.py`
double-stamping**: it was M3-era code that `stamp_image()`d a raw hex (CRC32 trailer,
`image_len=len(body)`). In M4 the app arrives already SHA256-stamped, so it clobbered `image_len`
(0x5FE8→0x604C) and replaced the sig trailer with CRC32. Fix = download the pre-stamped hex
*verbatim* (pad-to-row only). Logged as bench finding **S4-1** in the bring-up plan + a feedback
memory. Lesson: dump the *actual artifact* and diff vs source BEFORE instrumenting the code under
test; a tool that worked last milestone isn't a verified precondition this milestone.

## Seam 5 — CURRENT WORK (TCB isolation)

**Goal:** turn ADR-0017 reason #2 (TCB isolation) from an assertion into a demonstrated boundary:
a memory-safety bug in the CM4 FBL's attacker-reachable CAN/UDS parser must not be able to
read/corrupt/operate the public key or the CRYPTO engine — the *bus fabric* refuses it,
independent of CM4's own code.

**User's agreed decisions (via AskUserQuestion):**
- Scope = **BOTH** boundaries: crypto PPU + key SMPU.
- Rigor = **FULL**: separate protection contexts (CM0+ trusted PC, CM4 untrusted PC) + **locked**
  SMPU/PPU master structs so CM4 can't disable the walls.

### DONE (committed to working tree, not yet git-committed)
1. **`docs/architecture/decisions/ADR-0020-tcb-isolation-protection-units.md`** — full design.
   Decisions: D1 bus-fabric (SMPU/PPU) over core-MPU (core-MPU is self-administered = safety not
   security); D2 two PCs assigned before `Cy_SysEnableCM4`; D3 SMPU over CM0+ image flash
   `0x1000_0000..0x1002_0000` (the compiled-in `crypto_pubkey_dev`), substance=integrity/write-deny
   since pubkey isn't secret; D4 PPU over CRYPTO MMIO (fixed vs programmable = TRM-verify); D5 lock
   master structs (the step that makes it real); D6 enforcement is bench-only (no host model), one
   host-testable slice = region-descriptor math, demo is CM4-code-initiated (NOT mdw — DAP is PC0).
2. **`shared/prot/include/prot_region.h`** + **`shared/prot/src/prot_region.c`** — the host-testable
   slice: `prot_encode_region_size(size)→size_code` (size==1<<(code+1); 256B=7, 64K=15, 128K=16) and
   `prot_region_cover(base,len,*out)` → smallest aligned power-of-2 region + 8-way subregion-disable
   mask + `exact` flag. No vendor headers. Target maps `size_code` onto `cy_en_prot_size_t`.
3. **`shared/prot/tests/test_prot_region.c`** — 8 Unity tests, **8/8 GREEN**. Headline: 128 KB CM0+
   region → exact region, mask 0. Followed red→green (was 6/8 red vs the stub).
4. **`Makefile`** — added `PROT_INC/PROT_SRC/PROT_TEST`, `.PHONY` + aggregate `test` +
   `test_prot_region` run target + build rule; added `shared/prot` to `LINT_SRC` and cppcheck `-I`.
5. **`docs/briefs/M4-bringup-plan.md`** — Seam 5 section rewritten to the agreed scope + status;
   also contains the new S4-1 finding + the two-part "process lesson" from the uds_flash bug.

### Key facts for the target config
- CM0+ image region: `0x1000_0000 .. 0x1002_0000` (`FLASH_CM0P_SIZE = 0x20000` = 128 KB), from
  `node_a_gateway/bootloader/proj_cm4/linker/fbl_cm4.ld`. `crypto_pubkey_dev[]` is compiled into
  `crypto_ops_cm0p.c` → lands in this region. Deny CM4 the whole 128 KB (CM4 needs none of it).
- CM4 FBL runs from `0x1002_0000`; app from `0x1004_0000`. FBL + app are both the **CM4** master →
  one PC boundary covers both (M5-ready: the app will have to ask CM0+ for MAC ops).
- CM0+ boot flow is in `node_a_gateway/bootloader/proj_cm0p/src/main_cm0p.c`. The walls must go up
  in `main()` **before line ~200 `Cy_SysEnableCM4(CM4_VECTOR_TABLE_ADDR)`** (0x10020000 absolute).
  That file also has: fixed-SRAM debug struct `CM0P_DBG` @ `0x0801F600`, a `HardFault_Handler`
  override, `cm0p_setup_srom_syscalls()` (S0-4 fix — needed or CM4 flash erase hangs), a
  `.cy_ramfunc` idle spin (S0-3 RWW fix). VERIFY_IMAGE debug struct `VDBG` @ `0x0801F640` is in
  `crypto_ops_cm0p.c` (currently a diagnostic — to be removed when scaffolding is torn down).

### NEXT STEPS (in order)
1. **Scaffold CM0+ `Cy_Prot_*` config** (target-only, compile-guarded, TRM-verify markers):
   - SMPU over the key region using `prot_region_cover(0x10000000, 0x20000)` → base/size_code/mask.
   - PPU over CRYPTO MMIO (prefer fixed PPU; programmable fallback — TRM-verify which exists).
   - Assign CM0+ trusted PC + CM4 untrusted PC (`Cy_Prot_ConfigBusMaster`/active-PC — TRM-verify
     exact calls + recommended PC numbers; PC0 reserved as bypass/manager).
   - Lock SMPU/PPU master sub-structs to the CM0+ PC (D5).
   - Install in `main_cm0p.c` BEFORE `Cy_SysEnableCM4`.
2. **CM4 fault-demo hook** (debug build only): a routine that reads a key byte / pokes a CRYPTO reg;
   confirm CM4's fault handler catches the BusFault/HardFault. Observe via OpenOCD (we debug CM4).
3. **Bench:** before-walls the CM4 access succeeds; after-walls it faults; CM0+ still verifies boot;
   append the silicon findings (S5-x) to the bring-up plan + ADR-0020 "Review history".
4. **Then Seam 6** (fault injection: dead CM0+ → verify times out → fail-safe stay-in-FBL), then
   **tear down all diagnostic scaffolding**: `CM0P_DBG`, `VDBG`, `HardFault` override,
   `CM0P_ERASE_TEST_SLEEP`, `FBL_M4_SEAM3_BRINGUP`, restore ramfunc idle cleanly.

### TRM-verify open items for step 1
- Recommended PC numbers for CM0+/CM4 + exact bus-master config + active-PC PDL calls on CYT2B7.
- Whether a fixed PPU region covers the CRYPTO block exactly, or a programmable PPU is needed.
- SMPU struct count/priority ordering (higher index = higher priority).
- That locking the master sub-structs genuinely stops CM4 from disabling the walls / re-entering PC0.

## Security constraints to preserve
- Private key `ec_p256_dev_private.pem` NEVER on device; gitignored via `*_private*`. `gen_dev_key.py`
  keeps the private PEM host-side only. Generated `test_signed_image.h` is gitignored.
- Public key (`crypto_pubkey_dev.h`, `CRYPTO_DEV_KEY_ID=1`) is compiled into the CM0+ image; that's
  fine (it's public) — Seam 5 protects its *integrity*, not confidentiality.

## First message to paste into the new chat
"Continuing M4 Seam 5 (TCB isolation) on the TRAVEO T2G project. Read
`docs/handoff/HANDOFF-M4-seam5.md` for full context. ADR-0020 + the host-tested `shared/prot`
region helper are done (8/8 green). Next: scaffold the CM0+ `Cy_Prot_*` config (SMPU key + CRYPTO
PPU + PC assignment + lock) before `Cy_SysEnableCM4`, then the CM4 fault-demo hook, then bench.
Go ahead and scaffold the target config skeleton with TRM-verify markers."
