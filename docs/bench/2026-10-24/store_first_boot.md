# S — Freshness store mount on boot (ADR-0024 V2/V3), 2026-10-24

M5B bench, checklist block "S" (`docs/project/weeks/2026-W43.md`). Read with the debugger after a normal boot
on both nodes. The normal images were reflashed after block K and are built from `m5b/secoc_improve` at `c2b3a5d`.

## Result

| Variable | Node A (CYT2B7, CM4) | Node B (CYT4BF, CM7_0) | Meaning |
|---|---|---|---|
| `g_secoc_store_status` | 0 | 0 | **OK**: a valid record was found at mount (1 = FACTORY, 2 = BROKEN, which fails closed) |
| `g_wflash_raw_status` | 5373960 = **0x520008** | 0x520008 | `CY_FLASH_DRV_FLASH_NOT_ERASED`: the read-back blank check found the mark programmed (see below) |
| `g_wflash_raw_dma_intr` | 1 | 1 | `CY_DMAC_INTR_COMPLETION`, no error bits |

**Pass on both nodes.**

## Why 0x520008 is the success value, not an error

`g_wflash_raw_status` holds the PDL status of the **last** flash operation only (`wflash_port.c`), not a running
error flag. Booting mounts the store, then `secoc_tx_boot` commits epoch + 1. Every commit (`secoc_store.c`
`store_commit`) ends with:

erase other slot → program body → program mark → **read back**: blank-check the mark, then DMA-read the record, check the CRC and compare.

When the mark word was programmed, the blank check correctly answers `FLASH_NOT_ERASED`
(`CY_FLASH_ID_ERROR + 0x8`, i.e. `0x14 << 18 | 2 << 16 | 0x8`). The DMA read then completes, and the CRC and
compare pass. If any step had failed, the commit would have returned false, and `g_secoc_store_status` and the
SecOC traffic would show it. So **0x520008 + COMPLETION + status 0** is the signature of a good commit.

The checklist criterion "`g_wflash_raw_status` = no error" was wrong for a last-operation variable. The
correct reading is: not an unexpected PDL error code, plus the DMA showing COMPLETION.

## Store status is OK, not FACTORY

Both nodes mounted **OK** after being reflashed in K and K3, so `program` doesn't erase the work-flash slots.
That's what the design needs: the freshness epoch has to survive a firmware update as well as a reset. The
FACTORY path (a blank store) wasn't exercised tonight; it's covered by the host tests.

## ADR-0024 verification items

- **V2: closed on both nodes.** The SROM erase, program and blank-check calls from the app core succeed: commits
  completed at boot, and the G2/G3 run persisted floors 5 → 8 across Node B resets (`g2_g3_reset_replay.md`).
- **V3: partly closed.** The 20 B M-DMA work-flash read completes on both nodes. Still open: an uncorrectable ECC
  error reported as `SRC_BUS_ERROR`, the fault subsystem not resetting on it, and torn-word injection with
  `WORK_ECC_INJ_EN` / `FLASHC_ECC_CTL`. These are not part of tonight's checklist; defer them explicitly in ADR-0024.
