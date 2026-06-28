# M1 Bootloader — Bring-up Log

A live journal of bringing the M1 FBL up on the CYT2B7 (TRAVEO™ T2G Body Entry).
The host-testable core (boot decision, CRC, boot-loop counter) was proven on x86
**before** the board arrived; everything below is what happened at the
**silicon / port seam** — where the design doc met the chip.

Format per entry: *what I expected · what the silicon did · what surprised me /
the lesson*. These are the "the datasheet said X, the chip did Y" moments.

---

## 1. The memory map isn't flat — TRAVEO T2G is dual-core at boot

- **Expected:** code flash as a flat FBL+app split from `0x1000_0000` — FBL at the
  base (~64 KB), app right above it at `0x1001_0000`.
- **Silicon:** the BSP linker tells the real story. The ROM boots the **CM0+**
  first; its prebuilt image owns the **first 128 KB** (`FLASH_CM0P_SIZE = 0x20000`),
  and the CM4 code — our FBL — actually starts at `0x1002_0000`
  (`.text` is placed at `ORIGIN(flash) + FLASH_CM0P_SIZE`). So `0x1001_0000`, my
  assumed app base, sits **inside the CM0+ region**.
- **Corrected map:** CM0+ `@0x1000_0000` (128 KB) · FBL (CM4) `@0x1002_0000` ·
  app (CM4) `@0x1004_0000`. `FBL_APP_FLASH_BASE` moved `0x1001_0000 → 0x1004_0000`.
- **Lesson:** "FBL + app" is a single-core mental model. On an asymmetric
  dual-core part the ROM→CM0+→CM4 chain means flash isn't yours to lay out flat —
  the CM0+ prebuilt is a fixed tenant in the first 128 KB. The design-doc map was
  marked *"verify in TRM"*; the **BSP linker was the TRM** in practice.

## 2. The app had to become a CM4-only image

- **Expected:** build the app like any MTB app, link it at the app base, flash it.
- **Silicon:** a normal MTB app bundles **its own** CM0+ prebuilt at `0x1000_0000`
  — which would overwrite the FBL's CM0+ when flashed.
- **Fix:** relocate the app's CM4 (one line — `FLASH_CM0P_SIZE = 0x40000` → CM4 at
  `0x1004_0000`) and **strip the CM0+ at build time**
  (`objcopy --remove-section=.cy_m0p_image`). The FBL provides CM0+; the app is a
  pure CM4 payload.
- **Lesson:** in an FBL+app split on a dual-core part, exactly **one** image
  carries the CM0+ boot. The other is CM4-only, addressed and stamped by hand.

## 3. The CM4 vector table is only `0x80` — header at `0x100`

- **Expected:** reserve a generous image-header slot (I first defaulted the stamp
  tool to `0x400`).
- **Silicon:** the CM4 vector table is `0x80` (32 entries — the CPU-interrupt
  model, not a 240-entry table). So `FBL_APP_HEADER_OFFSET = 0x100` already clears
  it; the stamp tool was aligned down to `0x100`.
- **Lesson:** the header offset only has to clear the *actual* vector table — on
  this M4 that's smaller than reflex suggests.

## 4. Power-on reset is a **bit**, not zero

- **Expected (ADR-0007 D2 draft):** a POR clears the reset-cause register, so
  `cause == 0` can itself mean power-on.
- **Silicon (`RES_CAUSE` @ `0x4026_1800`):** POR is **bit 30 (`RESET_PORVDDD`)**,
  and the register's **default is `0x4000_0000`** — bit 30 *set*. So `cause == 0`
  is *not* power-on; it's the absence of any recorded cause. My draft mapping would
  have misfiled a real POR as `OTHER`.
- **Fix:** detect the "came up cold" group by bits — `PORVDDD | BOD* | XRES` — not
  by zero. ADR-0007 D2 corrected; verified live (XRES / power-cycle → `POWER_ON`).
- **Lesson:** textbook "datasheet said X, chip said Y." The reset-register-clears-
  to-zero intuition is simply wrong here — the register defaults non-zero. Read the
  bit-field; never assume the zero state.

## 5. A USB power cycle doesn't cold-boot the backup domain

- **Expected:** unplug/replug USB = a true cold power-off → the backup domain
  (`BREG`) clears, so the boot-loop counter resets to 0.
- **Silicon:** `BREG` **survived** the USB power cycle — the counter persisted
  across unplug/replug. Bulk capacitance / VBACKUP keeps the always-on domain alive
  through a bench blip.
- **Lesson:** a bench USB power-cycle **≠** a field cold-start (ignition off). When
  validating "what clears on power loss," a USB blip is the wrong stimulus. (The FBL
  still clears the counter correctly — on the firmware `POWER_ON` path, not by
  relying on hardware clearing `BREG`.)

## 6. The vendor driver bricked the boot — and **only without a debugger**

The big one. After the boot-loop demo, a power cycle parked the FBL in a HardFault.

- **The fault frame:** `CFSR = 0` (no bus/mem/usage fault), `HFSR = 0x8000_0000`
  (`DEBUGEVT`) — i.e. a **`BKPT` instruction**. `addr2line` on the FBL ELF resolved
  the stacked PC to `Cy_SysPm_BackupWordReStore`, called from `fbl_port_backup_read`,
  with `wordIndex = 0` (the stacked `r0`).
- **Root cause:** the PDL's `Cy_SysPm_BackupWord{Store,ReStore}` assert
  `CY_ASSERT_L3(index > 0)` — they **reject `BREG[0]`** — even though their own API
  doc says *"the index starts with 0."* The assert contradicts the contract; our
  `FBL_BREG_MAGIC_IDX = 0` tripped it on every call.
- **Why "only after a power cycle":** the assert *is* a `BKPT`. With a debugger
  attached (`C_DEBUGEN = 1`) a `BKPT` just **halts** — you resume past it and
  everything appears to work (the magic still got written, the counter still
  cleared). **Detached** (after a power cycle, KitProg3 lost USB power) the same
  `BKPT` escalates to a `DEBUGEVT` HardFault — and bricks the boot.
- **Fix:** access `BACKUP->BREG[idx]` directly — exactly what the PDL does
  internally (`BACKUP_BREG[i] = …`), minus the spurious assert.
- **Lessons:**
  1. **Read the driver source, not just the header.** The assert's intent wasn't in
     the docs — and disagreed with them.
  2. **"Works under the debugger, faults standalone" is a whole bug category.** A
     `BKPT` is benign attached and fatal detached. Always run a final **no-debugger**
     pass on real power.
  3. The host-testable core was innocent — the bug lived entirely at the port seam.

## 7. The handover must hand over a **clean interrupt state**

- **Expected:** the VTOR/MSP/branch jump *is* the whole handover.
- **Reality (design review + B3 contract):** the de-init was a stub — it disabled
  IRQs but never stopped SysTick or cleared pending NVIC. A clean POR-path jump
  works; a jump taken **after the app had already run** (a software reset) carries
  live interrupt state into the app's not-yet-initialised vector table → fault.
  *(This wasn't today's actual fault — that was #6 — but it's a real latent bug the
  review caught.)*
- **Fix:** stop SysTick, clear SysTick/PendSV pending, disable + un-pend every NVIC
  bank **before** the VTOR switch.
- **Lesson:** a bootloader→app jump is a **state hand-over**, not just a branch. The
  classic "works from cold boot but not after a warm path" failure is exactly
  carrying dirty interrupt state across VTOR.

---

## Meta-lesson

The host-testable-core split (ADR-0001) paid off exactly as designed: the boot
**decision** logic, the CRC, and the boot-loop counter rules were all correct on the
first silicon run. **Every** surprise above lived at the hardware / port seam — the
dual-core memory map, a reset-cause bit, a vendor-driver assert, the interrupt-state
handover. That's the seam that deserves the most attention on bring-up, and the host
tests are what let me trust everything behind it.

## Still open at the M1 tag (deferred)

- **`.noinit` on-target persistence + ECC priming.** Designed and host-tested; the
  ECC-fault-on-uninitialised-read behaviour (ADR-0007) is still a *verify-in-TRM*
  item. Its real consumer (the App↔FBL programming-request channel) is M3.
- **CAN knock window.** Designed (ADR-0008); needs a CAN interface on the bench to
  exercise. An M1 stretch goal, not a brief-scope item.
