# FBL port map — `shared/boot` (portable) ↔ `node_a_gateway/bootloader` (target)

How the host-testable boot core binds to the chip. The core (`shared/boot`) calls the
`fbl_port_*` interface in `shared/boot/include/fbl_port.h`; this directory provides the
**target implementation**, and `shared/boot/tests/boot_port_fake.c` provides the **host fake**.
Same interface, two backends — link-time selected (ADR-0001).

> The function names below are derived from ADR-0007/0008. **Match them to the actual
> signatures in your `fbl_port.h`** — if they differ, the header wins; this is the mapping,
> not a redefinition. Let Claude Code generate the stubs from `fbl_port.h` + this map.

## Rule of thumb
**Decides → `shared/boot`. Touches the chip → here.** Nothing in this directory's logic
should live in the core; nothing in the core should include a vendor header.

## Target file layout (`node_a_gateway/bootloader/`)

```
config/   fbl_config.h        T_knock, boot-loop N, region addresses, CAN knock ID, FBL_DIGEST_ALGO
linker/   fbl.ld              FBL flash region, .noinit placement, vector table at flash base
src/
  fbl_main.c                  FBL entry / super-loop; wires the shared/boot core to the port
  startup_fbl.(S|c)           FBL vector table at flash base (from ModusToolbox, customised)
  port_reset.c                reset-cause read/clear
  port_noinit.c               .noinit region accessor + ECC priming
  port_backup.c               backup-domain (BREG) read/write
  port_time.c                 monotonic millisecond clock
  port_can.c                  knock-frame poll (FBL minimal CAN bring-up)
  port_security.c             device-lifecycle read
  port_image.c                app-image/flash read + optional HW-CRC
  port_jump.c (+ asm)         de-init + the naked/asm VTOR/MSP/branch
```

## Function → file → hardware → host fake

| `fbl_port_*` (per ADR) | Target file | Touches (verify in TRM) | Host fake (`boot_port_fake.c`) |
|---|---|---|---|
| `reset_cause` / `clear_reset_cause` | `port_reset.c` | SRSS `RES_CAUSE`/`RES_CAUSE2`; clear after read; POR may read 0 | settable cause value |
| `noinit_region` | `port_noinit.c` | base/size of the `.noinit` SRAM region (from linker) | RAM buffer |
| `prime_noinit` | `port_noinit.c` | ECC-priming write over the region at ECC-word granularity | memset the buffer |
| `backup_read` / `backup_write` | `port_backup.c` | BACKUP block BREG[] (backup domain; survives hibernate) | register array |
| `now_ms` | `port_time.c` | SysTick or a free-running timer | settable/advanceable clock |
| `tool_contact` | `port_can.c` | CAN FD: poll for the knock frame on the diag ID (→ UDS DSC in M3) | scripted contact timeline |
| `lifecycle` | `port_security.c` | device lifecycle stage (eFuse/SFLASH) — gates the knock window | settable stage |
| `app_image` / flash read | `port_image.c` | code flash is memory-mapped (pointer + len); optional HW-CRC via crypto block | RAM buffer = fake image |
| `deinit_for_jump` (D4 step 3) | `port_jump.c` | `__disable_irq`, stop SysTick (+clear `ICSR` pend), NVIC `ICER`/`ICPR`, peripherals→reset | no-op |
| `jump_to_app(msp, reset)` (D4 5–7) | `port_jump.c` (naked/asm) | set `VTOR`, `__set_MSP`, `DSB`/`ISB`, `BX` — nothing stack-dependent between MSP-set and branch | records (msp,reset) for assertions |

Notes:
- **The jump is two pieces** (ADR-0008 D4): `deinit_for_jump()` is ordinary C; the final
  MSP-set + branch is a small **naked/asm** helper so the compiler inserts no stack use
  between `__set_MSP` and `BX`. Per the B3 handover contract: leave all IRQ sources
  disabled + pending cleared, `VTOR`/`MSP`/`CONTROL` set; the **app** re-enables and re-inits.
- **`port_image.c`** mainly hands the core a pointer+length; the digest *check* and vector
  sanity are core logic (host-tested). HW-CRC is an optional acceleration, not required for M1.
- **CAN config** is the bootloader variant of `shared/can` (polled + minimal) — `port_can.c`
  brings it up just enough to poll for the knock frame.

## Build & lint scope
- These files are **target-only**: built by ModusToolbox, **not** the host `Makefile`/CI.
  Only `shared/boot` is host-built and unit-tested.
- Exclude `vendor/` (and target startup) from cppcheck/MISRA — third-party / generated code
  isn't yours to hold to the standard. Keep the FBL port itself in scope (it's your code).
