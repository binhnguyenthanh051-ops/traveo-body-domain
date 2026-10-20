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
it; (3) program words 0–3; (4) program word 4 (`COMMIT_MARK`); (5) read the target back and
check it as D3 would; (6) only then return `true`. Any failure ⇒ `false`, and the mirror is
rolled back to the newest valid record.

**Power-loss argument:** the newest valid record is never touched by a save, and the
target only becomes valid when its last word is programmed. A power loss at any point leaves
the previous record as the newest valid one, so `load` returns the pre-save value. Because
`commit` never returned, that value was never spent (REQ-SECOC-006 "commit before spend"). At
worst one epoch is wasted. Never reused.

### D3. Load order: blank-check the mark, read the mark by DMA, then read the body by CPU

For each slot:

1. **`Cy_Flash_BlankCheck`** on word 4. Blank ⇒ the save never reached its last step ⇒ slot
   **empty**. Nothing else in the slot is read (hazard 1).
2. Not blank ⇒ read word 4 with a **single-word DMA transfer**, ECC left **on** (TRM §9.3.1
   measure 1). An uncorrectable error is reported as a DMA error status (M-DMA
   `DMAC_CHx_INTR.SRC_BUS_ERROR`, or P-DMA `CH_STATUS.INTR_CAUSE = 2`), not as a CPU fault.
   DMA error, or a value `!= COMMIT_MARK` ⇒ the mark itself was torn ⇒ slot **torn**
   (hazard 2). A single-bit error in the mark is still corrected by ECC on this path.
3. Mark intact ⇒ words 0–3 were fully programmed before it (D2 order) ⇒ read them with **ECC
   on** (single-bit errors corrected) and check the CRC. Bad CRC ⇒ slot **corrupt**.
4. Otherwise the slot is **valid**.

Result: two valid ⇒ the higher `seq` wins. One valid ⇒ use it. None valid and **both empty** ⇒
factory-fresh: every domain cold (D8's existing "cold ⇒ E = 0" path). None valid and **any
torn or corrupt** ⇒ `load` **fails** and the node treats the store as broken. The caller's
existing fail-safe applies (`secoc_tx_boot` / `secoc_rx_boot` return false ⇒ no SecOC traffic,
`LOG_EVT_CRYPTO_ERROR`-style report). A store that falls back to "cold" after corruption would
restart the epoch at 0 and reuse every epoch, which is exactly what REQ-SECOC-006 forbids.

**Owner decision (2026-10-20):** blank check is the primary mechanism wherever the driver
supports it (it does, on both PDL versions). For what blank check can't cover, a torn mark,
the mark is read by **DMA with ECC on** rather than by disabling work-flash ECC. ECC stays
enabled throughout, so single-bit errors are corrected and nothing is toggled globally. The
DMA read covers one word per slot (the mark), never the body.

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
models erased words, torn words (a word that faults on an ECC-on read) and power loss at any
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
  matters, and it plays out in two ways:
  - **In the mark** (read by DMA): the slot reads as torn and `load` falls back to the older
    record. For the sender, the D5 resync heals it (the peer's floor pushes the epoch past the
    lost value). For the receiver, its floor drops by one save, so frames from the lost epoch
    would be accepted again until the next adoption.
  - **In the body** (read by the CPU, D3 step 3): an uncorrectable ECC fault on the CPU at
    boot, not a fallback. If the fault handler resets, the node can boot-loop. Reading the
    whole 20 B record by DMA, not just the mark, would turn this into the fallback case too;
    it's left as an open follow-up (the ADR scope follows the owner's decision for the mark
    word). Cost: 16 more bytes in the same DMA transfer.

  Both are accepted for a portfolio node and noted, not engineered away.
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
- **Disable work-flash ECC for the mark read** (`FLASHC_FLASH_CTL.WORK_ECC_EN = 0`, interrupts
  masked, restored right after). No DMA channel needed, a few register writes. Not chosen
  (owner decision, 2026-10-20): `WORK_ECC_EN` is a global switch shared by every core and
  master, a single-bit error in the mark is no longer corrected (so a healthy record can be
  rejected and the older one used, the residual-risk case below), and the toggle window is one
  more thing that can go wrong. DMA keeps ECC on. Revisit if a DMA channel can't be spared on
  a node.
- **Route uncorrectable ECC faults to one core and recover in the handler** (TRM §9.3.1
  measure 2), or **set the fault action to reset** (measure 3). Rejected: a fault handler that
  resumes after a bus error is hard to get right and to test, and reset-on-fault turns a torn
  record into a boot loop.
- **Defer to M6** (REQ-SECOC-006/007/008 "host-only, deferred"). Rejected at the W42 retro:
  breaks the published M5a promise for longer, and the reset-then-replay demo stays broken.

## To verify in the TRM / on silicon

- **V1** Node B (CYT4BF) small-sector size, endurance, ECC width and SROM path: Body High TRM
  (add it to `docs/references/`). Wed 21.
- **V2** `Cy_Flash_BlankCheck` from the app core goes through the CM0+ SROM path on both nodes.
  Our CM0+ images service SecOC/crypto IPC, so confirm the system-call IPC still reaches SROM
  (Node A did erase/program from the CM4 in M3; Node B is unproven). Wed 21.
- **V3** DMA read of work flash: which DMA (M-DMA or P-DMA) is free on each node, that a
  single-word transfer from work flash reports an uncorrectable error in its status as TRM
  §9.3.1 says, and that a torn mark can be simulated on target with the fault-injection registers
  (`WORK_ECC_INJ_EN`, `FLASHC_ECC_CTL`) for a bench test. Wed 21.
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
