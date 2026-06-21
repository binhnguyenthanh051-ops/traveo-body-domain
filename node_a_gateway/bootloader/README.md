# node_a_gateway/bootloader — Flash Bootloader (FBL)

Deliberately minimal (super-loop / cooperative), small attack surface. Owns: ROM-anchored
secure boot follow-through, app image **verification**, the no-init RAM handshake, the
VTOR/MSP jump to the app, and UDS **programming** services. Built with ModusToolbox.

Pulls from `shared/` (can, diag, crypto, hal) with a bootloader config. See
`docs/architecture/overview.md` §3 and ADR-0004/0006. MVP = milestone M1.

## Layout

The boot **decision logic** is host-tested in `shared/boot` (ADR-0001); this directory is
the **target binding** — same `fbl_port.h` interface, real hardware backend. See
`PORT_MAP.md` for the function→file→hardware→fake mapping.

- `config/fbl_config.h` — bootloader-variant config (digest algo, knock window, N, knock CAN ID)
- `linker/fbl.ld` — FBL flash region, vector table at flash base, pinned `.noinit` (verify in TRM)
- `src/fbl_main.c` — entry; runs `fbl_run_boot()` then jumps or enters programming mode
- `src/startup_fbl.c` — startup placeholder (MTB-generated on target; notes the FBL customisations)
- `src/port_*.c` — the `fbl_port_*` backends, one per hardware block

**Status (M1):** port scaffolded. Board-independent pieces written (`port_noinit` accessor,
`port_backup` skeleton, `port_image`, `port_jump` de-init + naked asm). Board-gated pieces
stubbed with safe defaults + TODOs (`port_reset`, `port_time`, `port_can`, `port_security`,
ECC priming, startup). Target-only — built by ModusToolbox, not the host `Makefile`/CI.
