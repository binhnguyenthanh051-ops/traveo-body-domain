# ADR-0024: Persistent freshness store — a two-slot record in work flash (M5B)

**Status:** proposed · **Date:** 2026-10-20

> Owns the **backing** for the `secoc_freshness_store_if_t` port (ADR-0021 D8): where the
> sender epoch and the receiver floor live across a power cycle, how a save survives a power
> loss, and how a load reads work flash without tripping an ECC fault. It replaces the
> RAM-backed store in both nodes' `secoc_app.c`, which leaves **REQ-SECOC-006/007/008** unmet on
> target (W43 gap list G1–G3). ADR-0021 D8 said "`eeprom_emu` backing in M6"; this ADR takes a
> smaller, dedicated store now and leaves `eeprom_emu` parked (see Alternatives).

## Context

- **What is stored:** per node, the **D8 domains**: the node's own sender epoch
  (`SECOC_DOM_TX_SELF`) and the floor for its one peer (`SECOC_DOM_RX_A` on Node B,
  `SECOC_DOM_RX_B` on Node A). Two `u16` values, plus whether each has ever been written
  (`load` must still be able to say "cold", D8).
- **How often:** once per boot (sender `commit(E+1)` before spending `E`), once per counter
  rollover (ADR-0021 D3, ~1.8 h at 100 ms), and once per adopted epoch on the receiver
  (`secoc_rx_accept`, rare). Roughly 10–20 saves a day in the car scenario ADR-0021 D3 uses.
- **The invariant (REQ-SECOC-006):** `commit` is durable **before it returns**. A brown-out
  may waste an epoch, never reuse one.
- **Work flash, Node A (CYT2B7):** from the Body Entry TRM, 002-19314 Rev. \*J, §9. Small sectors
  are 128 B, large sectors 2 KB. Programming is in **32-bit** units, with SECDED ECC (7 bits)
  per 32-bit word, so a word can't be reprogrammed without an erase. Endurance is 250 k cycles,
  retention 10 years. Erase and program are **SROM APIs executed by the CM0+ via IPC** (§9.3.2).
- **Work flash, Node B (CYT4BF):** the PDL device header (`cyt4bf8cds.h`) maps a 32 KB
  small-sector region at `CY_WFLASH_SM_DBM0_BASE` (`0x1401_8000`). Small-sector size,
  endurance and ECC width come from the Body High TRM, which is **not in `docs/references/`
  yet**: confirm before the Node B port is written (open item V1).
- **The read hazards (TRM §9.3.1):**
  1. Reading an **erased** word returns random data **and raises an ECC error**. The TRM's
     answer: blank-check first.
  2. After a power loss mid-program, "the flash state will be unknown". A **half-programmed**
     word is *not blank*, and its ECC can be uncorrectable, so blank check alone doesn't make it
     safe to read.
  3. An ECC fault is reported to **one** CPU only.
- **Driver:** both nodes use the eCT flash IP (`CY_IP_MXFLASHC_VERSION_ECT`, already relied on
  in ADR-0012 S1). PDL `cy_flash.h` (v3.22.1 Node A, v3.23.0 Node B) provides
  `Cy_Flash_EraseSector`, `Cy_Flash_ProgramRow` and **`Cy_Flash_BlankCheck`**
  (`addrToBeChecked`, `numOfWordsToBeChecked`), all behind that define. Work-flash ECC is
  switched by `FLASHC_FLASH_CTL.WORK_ECC_EN` (TRM §9.2.2.2: when 0, "no correctable or
  non-correctable faults are reported").

## Decisions

### D1. Two slots, one record per node, both domains in every record

Two work-flash **small sectors** (slot 0 and slot 1), each holding at most one record. A record
carries **both** domains, so a save always rewrites the whole node state. Layout, in 32-bit
words (word = program unit):

| Word | Content |
|---|---|
| 0 | `seq` (u32): save sequence number, +1 per save |
| 1 | `epoch[TX_SELF]` (u16, low half) ∥ `epoch[RX_peer]` (u16, high half) |
| 2 | `written` bitmap (bit *n* = domain *n* has been committed at least once), rest 0 |
| 3 | CRC-32 (IEEE 802.3) over words 0–2 |
| 4 | `COMMIT_MARK` = `0xA5C3_3C5A`, programmed **last** |

20 B per record, fitting a 128 B small sector with room to grow. The store keeps a RAM mirror
of the newest record. `commit(domain, epoch)` updates the mirror and saves it; `load(domain)`
answers from the mirror (`written` bit clear ⇒ cold, as D8 requires).

### D2. Save order: erase the older slot, program body then mark, read back, then return

`commit` = (1) pick the **target** = the slot that is *not* the newest valid one; (2) erase
it; (3) program words 0–3; (4) program word 4 (`COMMIT_MARK`); (5) read the target back **by DMA**
and check it as D3 would; (6) only then return `true`. Any failure ⇒ `false`, and the mirror is
rolled back to the newest valid record.

**Power-loss argument:** the newest valid record is never touched by a save, and the
target only becomes valid when its last word is programmed. A power loss at any point leaves
the previous record as the newest valid one, so `load` returns the pre-save value. Because
`commit` never returned, that value was never spent (REQ-SECOC-006 "commit before spend"). At
worst one epoch is wasted. Never reused.

### D3. Load order: blank-check the mark, then read the whole record by DMA

For each slot:

1. **`Cy_Flash_BlankCheck`** on word 4. Blank ⇒ the save never reached its last step ⇒ slot
   **empty**. Nothing else in the slot is read (hazard 1).
2. Not blank ⇒ read words 0–4 (the whole 20 B record) in **one DMA transfer**, ECC left **on**
   (TRM §9.3.1 measure 1). ECC still checks every word on this path. What changes is who gets
   an uncorrectable error: on a CPU read it's a bus fault, on a DMA read it's the channel's
   error status (M-DMA `DMAC_CHx_INTR.SRC_BUS_ERROR`, P-DMA `CH_STATUS.INTR_CAUSE = 2`), which
   software reads as data. Single-bit errors are corrected on both paths.
3. Classify: DMA error ⇒ slot **torn** (hazard 2, anywhere in the record). Word 4
   `!= COMMIT_MARK` ⇒ **torn** (the mark was mid-program). Bad CRC over words 0–2 ⇒
   **corrupt**. Otherwise **valid**.

The CPU never reads a store slot directly, so no state of work flash (erased, torn or
decayed) can fault the CPU.

Result: two valid ⇒ the higher `seq` wins. One valid ⇒ use it. None valid and **both empty** ⇒
factory-fresh: every domain cold (D8's existing "cold ⇒ E = 0" path). None valid and **any
torn or corrupt** ⇒ the store is **BROKEN** and fails closed. A store that fell back to "cold"
after corruption would restart the epoch at 0 and reuse every epoch, which is exactly what
REQ-SECOC-006 forbids.

**How BROKEN reaches the caller (implementation, 2026-10-20):** the D8 port can't carry it,
because `load` returning false already means *cold*. So the scan runs once in
`secoc_store_mount()`, which returns `OK` / `FACTORY` / `BROKEN`. On BROKEN, `load` and `commit`
both return false, so `secoc_tx_boot` fails (its commit fails). But `secoc_rx_boot` only
*loads*, and would boot cold with floor 1 and accept every old epoch. The app therefore
**must check the mount status** and not start SecOC at all on BROKEN. The D8 port is unchanged.

**A brown-out during the very first save** (factory flash) depends on the step: interrupted
while programming the body, the mark is still blank, so both slots read empty and the store is
still FACTORY (nothing was spent). Interrupted during the erase or the mark program, it leaves
a non-blank torn slot next to an empty one. That's BROKEN, because it can't be told apart from
"the only record decayed". Recovery is erasing both slots (debugger or a future UDS routine).
Writing an initial record at production programming time would remove this window. Not done
for M5B; noted as a provisioning step.

**Owner decision (2026-10-20):** blank check is the primary mechanism wherever the driver
supports it (it does, on both PDL versions). For what blank check can't cover, a torn or
decayed word, the record is read by **DMA with ECC on** rather than by disabling work-flash
ECC. ECC stays enabled throughout, so single-bit errors are corrected and nothing is toggled
globally. **Revised the same day:** the first draft read only the mark by DMA and the body by
CPU, which left a multi-bit body error as a CPU fault at boot. The whole record now goes by DMA
(16 more bytes in the same transfer).

### D4. Behind the HAL: `hal_flash_if_t` gets two optional entries

```c
/* Optional (NULL if unsupported). 1 = blank, 0 = programmed, <0 = error. */
int (*blank_check)(uint32_t addr, size_t len);
/* Optional. Fault-safe read (ADR-0024 D3 step 2): an uncorrectable ECC error
 * returns <0 instead of faulting the CPU. On target: a DMA transfer. */
int (*read_checked)(uint32_t addr, uint8_t *dst, size_t len);
```

The same pattern as `erase_range` (ADR-0012 D6): existing instances (the M3 download flash) set
them to NULL and are unaffected. The store logic (`shared/secoc_store/` or a new file in
`shared/secoc/`, decided at implementation) is **host-testable** (rule 1): the host fake flash
models erased words, torn words (`read_checked` returns <0 over them) and power loss at any
program/erase step. The target port wraps `Cy_Flash_*` and one DMA channel for
`read_checked`. The interface names what the read guarantees (fault-safe), not how, so the
port could switch mechanism without changing the store.

### D5. One writer, serialized

Sender saves (boot, rollover) and receiver saves (epoch adoption) can come from different
tasks on the same core. The target port serializes `commit` with a mutex, and only the **app
core** reads the store, so hazard 3 (one ECC-fault recipient) doesn't arise. The CM0+ executes
the SROM erase/program on the app core's behalf (TRM §9.3.2) but never reads the store.

## Consequences

- (+) REQ-SECOC-006/007/008 become satisfiable on target: the epoch and floor survive power
  cycles, and the reset-then-replay demo (W43 Sat, G2) can be refused.
- (+) The power-loss argument fits in one paragraph (D2) and is host-testable step by step.
- (+) No new dependency. It uses PDL calls already proven on Node A in M3 (ADR-0012 S1).
- (−) Not general-purpose. A second persisted value needs a record-format change. `eeprom_emu`
  stays the long-term answer.
- (−) Every save is an erase. Commit latency is erase + 5 word programs (datasheet numbers,
  to measure on Saturday), and a receiver save sits in the RX path. That's acceptable because
  it fires once per adopted epoch, not per frame.
- (−) **Residual risk, stated plainly:** a record can decay *after* it was written correctly.
  Every word has SECDED and retention is specified at 10 years, so only a multi-bit error
  matters. With the whole record read by DMA, it makes the newest slot read as torn or corrupt,
  and `load` falls back to the older record. For the sender, the D5 resync heals it (the peer's
  floor pushes the epoch past the lost value). For the receiver, its floor drops by one save,
  so frames from the lost epoch would be accepted again until the next adoption. Accepted for
  a portfolio node and noted, not engineered away.
- **Wear:** about 10 saves a day per sector (two slots) against 250 k cycles is decades. The
  binding limit is the **u16 epoch** (ADR-0021 D3): 65 536 increments at ~20 a day is under 9
  years. The store doesn't change that limit, but it makes it real, so ADR-0021 D3's "on the
  edge" note now has a number.

## Alternatives considered

- **Append-only log** (records written into erased space, sector erased only when full,
  ping-ponging two sectors). Fewer erases and faster saves. Rejected: more code (torn-record
  scan, sector switch) for a wear benefit we don't need (see Wear).
- **Full `eeprom_emu`** (parked module, `plan.md`). Reusable. Rejected for M5B: several weeks of
  work. Revisit when a second consumer needs persistence.
- **Vendor EEPROM-emulation middleware.** Vetted. Not chosen: an extra dependency to evaluate,
  and its power-loss argument would be the vendor's, not ours to explain. Not checked whether a
  TRAVEO T2G variant exists.
- **RAM that survives reset (`.noinit`) or backup registers.** No flash code. Rejected: lost on
  power-off, so REQ-SECOC-006 fails the power-cycle test. It would only cover a warm reset.
- **No persistence: a receiver nonce at boot, or per-session keys.** Removes the store
  entirely. Out of scope: it's the post-M5 session-key milestone.
- **Disable work-flash ECC for the read** (`FLASHC_FLASH_CTL.WORK_ECC_EN = 0`, interrupts
  masked, restored right after). No DMA channel needed, a few register writes. Not chosen
  (owner decision, 2026-10-20): `WORK_ECC_EN` is a global switch shared by every core and
  master, a single-bit error is no longer corrected (so a healthy record can be
  rejected and the older one used, the residual-risk case below), and the toggle window is one
  more thing that can go wrong. DMA keeps ECC on. Revisit if a DMA channel can't be spared on
  a node.
- **Route uncorrectable ECC faults to one core and recover in the handler** (TRM §9.3.1
  measure 2), or **set the fault action to reset** (measure 3). Rejected: a fault handler that
  resumes after a bus error is hard to get right and to test, and reset-on-fault turns a torn
  record into a boot loop.
- **Defer to M6** (REQ-SECOC-006/007/008 "host-only, deferred"). Rejected at the W42 retro:
  breaks the published M5a promise for longer, and the reset-then-replay demo stays broken.

## Target ports (2026-10-21)

One port source, `shared/secoc_store/port/wflash_port.c`, serves both nodes. What differs is
config data in each `secoc_app.c` (ADR-0004), not `#ifdef`:

| | Node A (CYT2B7, CM4) | Node B (CYT4BF, CM7_0) |
|---|---|---|
| Slots | `CY_WFLASH_SM_SBM_BASE` (`0x1401_2000`) and +2 KB | `CY_WFLASH_SM_SBM_BASE` (`0x1403_0000`) and +2 KB |
| Small sector | 128 B (Body Entry TRM §9) | 128 B assumed, **unverified** (V1) |
| M-DMA trigger | `TRIG_OUT_MUX_2_MDMA_TR_IN0`, channel 0 | `TRIG_OUT_MUX_3_MDMA_TR_IN0`, channel 0 |
| DMA/SROM scratch | plain RAM (no D-cache) | `.cy_sharedmem` → `ram_noncache` (ADR-0018 D6) |

- **Slots 2 KB apart** in the small-sector region, so they're separate sectors whether a small
  sector is 128 B or 2 KB. That makes the Node B layout safe before V1 is answered.
- **PDL calls:** `Cy_Flash_EraseSector`, `Cy_Flash_Program_WorkFlash` (one 32-bit word per call,
  with the SROM's own blank check, so a non-erased word is refused), `Cy_Flash_BlankCheck`
  (`CY_FLASH_DRV_SUCCESS` = blank, `CY_FLASH_DRV_FLASH_NOT_ERASED` = not blank, TRM SROM status
  `0xF00000A4`), and one M-DMA 1D transfer for `read_checked`. `Cy_Flashc_WorkWriteEnable()`
  lifts the work-flash write gate at init, like M3's `MainWriteEnable`.
- **Interrupts stay enabled** around erase and program. M3's code-flash port masks them because
  an ISR fetching from the busy code-flash macro faults; nothing runs from work flash.
- **Cache (Node B):** the M-DMA reads the descriptor and writes the read buffer, and the CM0+
  (SROM) reads the program source word. None of them sees the CM7's D-cache, so all three live
  in `.cy_sharedmem`. Compiled: the 0x60-byte scratch lands in `.cy_sharedmem` at 32-byte
  alignment.
- **BROKEN at boot:** `secoc_app_init()` skips `secoc_tx_boot`/`secoc_rx_boot`, so nothing goes
  out authenticated and every received frame is dropped as an unknown ID.
  `g_secoc_store_status` (0 OK, 1 FACTORY, 2 BROKEN) is there for the debugger.
- **Builds:** Node A app builds and links (39.6 KB flash, store and port symbols present).
  Node B: the three changed files compile clean (CM7 flags of the last good build, plus
  `-Wextra`), but the full `build_proj` couldn't run from Claude's shell (the device-configurator
  step fails before compiling). **The Node B link is still to be done** from ModusToolbox.

## To verify in the TRM / on silicon

- **V1** Node B (CYT4BF) small-sector size, endurance and ECC width: Body High TRM (add it to
  `docs/references/`). The slot layout above doesn't depend on the answer; `SECOC_WF_SECTOR`
  in Node B's `secoc_app.c` should match it.
- **V2** The SROM calls (erase, program, blank check) from the app core reach the CM0+ and
  succeed on both nodes. Node A did erase/program from the CM4 in M3 (code flash); work flash
  and Node B are unproven. Read `g_wflash_raw_status` on the first boot.
- **V3** The M-DMA read: a 20 B work-flash transfer completes on both nodes
  (`g_wflash_raw_dma_intr` shows COMPLETION), an uncorrectable error reports `SRC_BUS_ERROR`
  as TRM §9.3.1 says, the fault subsystem isn't set to reset on the same error, and a torn word
  can be simulated with the fault-injection registers (`WORK_ECC_INJ_EN`, `FLASHC_ECC_CTL`).
- **V4** Commit latency (erase + program) measured on both nodes. Sat 24, with the MAC latency if
  time.
- **V5** Power-cycle both nodes: epoch advances and never repeats (G1). Reset Node B and replay
  an old `0x120`: refused (G2). Sat 24.

## Traceability

- Serves **REQ-SECOC-006** (D1–D3), **REQ-SECOC-007** (receiver floor persisted, D1),
  **REQ-SECOC-008** (floor = persisted + 1 after reset needs a real persisted value).
- Implements the **ADR-0021 D8** port. Supersedes D8's "`eeprom_emu` backing in M6" line
  (ADR-0021 updated to point here).
- Host tests (Tue 20): durable before spend, power loss at every erase/program step wastes at
  most one epoch, never reuses one, torn mark ⇒ the other slot wins, both torn ⇒ `load` fails
  (not cold).
