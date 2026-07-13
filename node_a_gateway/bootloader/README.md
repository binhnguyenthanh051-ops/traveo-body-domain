# node_a_gateway/bootloader — Flash Bootloader (FBL)

Deliberately minimal (super-loop / cooperative), small attack surface. Owns: ROM-anchored
secure boot follow-through, app image **verification**, the no-init RAM handshake, the
VTOR/MSP jump to the app, and UDS **programming** services. Built with ModusToolbox.

Pulls from `shared/` (can, diag, crypto, hal) with a bootloader config. See
`docs/architecture/overview.md` §3 and ADR-0004/0006. MVP = milestone M1.

## Layout

Since M4 Seam 0 (ADR-0017 D4 / `docs/briefs/M4-bringup-plan.md`), this is an MTB
**APPLICATION** with two **PROJECT**s, not a single COMBINED project — the CM0+ crypto
service needs a real, buildable sibling to the FBL instead of the vendor's linked-in
prebuilt. `make build`/`make program` from *here* still build/flash both; each project
also builds standalone from within its own directory.

- `proj_cm4/` — the FBL itself (everything below was here directly through M1–M3).
  - `config/fbl_config.h` — bootloader-variant config (digest algo, knock window, N, knock CAN ID)
  - `linker/fbl_cm4.ld` — FBL flash region, vector table, pinned `.noinit` (ADR-0007 D8)
  - `src/fbl_main.c` — entry; runs `fbl_run_boot()` then jumps or enters programming mode
  - `src/startup_fbl.c` — startup placeholder (MTB-generated on target; notes the FBL customisations)
  - `src/port_*.c` — the `fbl_port_*` backends, one per hardware block
- `proj_cm0p/` — the crypto-service CM0+ image (ADR-0017). At Seam 0 it only starts the
  CM4 (`Cy_SysEnableCM4`) and idles — replacing the vendor `COMPONENT_CM0P_SLEEP` prebuilt
  with our own buildable image before any IPC/crypto lands on top (Seam 1+).

The boot **decision logic** is host-tested in `shared/boot` (ADR-0001); `proj_cm4` is the
**target binding** — same `fbl_port.h` interface, real hardware backend. See
`PORT_MAP.md` for the function→file→hardware→fake mapping (paths there are relative to
`proj_cm4/`, unchanged in content by the Seam-0 split).

**Status (M1):** port scaffolded. Board-independent pieces written (`port_noinit` accessor,
`port_backup` skeleton, `port_image`, `port_jump` de-init + naked asm). Board-gated pieces
stubbed with safe defaults + TODOs (`port_reset`, `port_time`, `port_can`, `port_security`,
ECC priming, startup). Target-only — built by ModusToolbox, not the host `Makefile`/CI.

**Status (M4 Seam 0):** two-project split scaffolded (this commit) — **unvalidated**, no
ModusToolbox install was available to run `make getlibs`/`make build` against it. Confirm
that before trusting it; see the note at the top of each new Makefile for exactly what to
check.
