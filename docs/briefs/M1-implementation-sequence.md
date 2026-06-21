# M1 implementation sequence

Short ordering note for building the bootloader MVP. The principle: **settle the portable
core (and its tests) before the target port, and do everything board-independent first** so
the still-shipping board never blocks you. Sources: ADR-0007, ADR-0008, the boot review
(`docs/reviews/ADR-0007-0008-review.md`), and `node_a_gateway/bootloader/PORT_MAP.md`.

## Step 0 — Clear the review findings that change the core (do first)
Both touch `shared/boot`, so settle them before anything builds on top.
- **B1** — single-source the boot-loop counter: remove the ADR-0008 step-4 increment; keep the
  reset-cause model in ADR-0007 D4. Update the ADR text + the core logic.
- **B4** — define the digest covered range to exclude the integrity fields (M1 digest; M4
  hash+sig). Update ADR-0008 D3 + the header layout.
- **B2** — make D4 explicit: sw-reset **with** `.noinit` programming pattern ⇒ clear counter;
  **without** ⇒ increment; classify hibernate-wake independent of the generic reset path.
- Tick these in the review checklist as you go; leave the review file in place.

## Step 1 — Finish the host-testable core (`shared/boot`, no board)
The whole decision tree, reset-cause classification, prime-vs-read + prime-bias, `.noinit`
parse/validate/init, BREG counter rule, header parse + digest + vector sanity, knock dwell.
- Drive it all from the Unity suite in `shared/boot/tests/` against `boot_port_fake.c`.
- Target the scenarios already listed in ADR-0008 (e.g. "cold + SECURE + valid ⇒ jump, no
  knock", "warm + counter>N ⇒ programming", "corrupt `.noinit` on read-cause ⇒ reinit").
- Exit criterion: `make test` + `make lint` green; the core compiles with **no vendor header**.

## Step 2 — Scaffold the target port stubs (`node_a_gateway/bootloader/`)
From `fbl_port.h` + `PORT_MAP.md`, generate the `port_*.c` files with TODO bodies — one per
hardware block. Add `fbl_main.c`, `config/fbl_config.h`, `linker/fbl.ld`, FBL startup.
- Each stub: correct signature, a `/* TODO(M1, board): ... */`, and a safe default/return so
  the FBL links. No real register access yet.
- These are target-only — not in host CI. Keep `vendor/` + startup out of lint; FBL port in.

## Step 3 — Implement the board-independent-enough port pieces now
Some port code can be written (if not fully run) before the board:
- `port_noinit.c` region accessor (addresses from the linker script).
- `port_backup.c` BREG read/write skeleton.
- `port_jump.c`: the `deinit_for_jump()` C + the naked/asm `jump_to_app` helper — write it now
  against the B3 handover contract; it just can't be *run* until the board.
Leave genuinely board-gated bits stubbed: ECC priming, SRSS reset-cause read, lifecycle read,
CAN knock poll.

## Step 4 — On-board bring-up (when the board arrives)
Generate the ModusToolbox project for CYT2B7, wire the shared modules, then fill the stubbed
port pieces and validate against the TRM items each ADR flagged:
- reset-cause register semantics + the `cause==0 ⇒ POR` assumption (ADR-0007);
- ECC behaviour on uninitialised SRAM + priming granularity (ADR-0007 D3);
- backup-domain retention across hibernate/POR (ADR-0007 D1; the B6 backup-map decision);
- the jump from **cold boot**, not just under the debugger (ADR-0008 D4 — the classic trap).

## Definition of done (M1)
FBL boots → reads handshake → checks app validity (CRC32) → jumps to a minimal app stub, from
a **cold boot**; boot-loop fallback + knock window work; ROM-secure-boot **designed** (burn
deferred to M4). Tag it: `git tag -a m1-bootloader -m "FBL boots, verifies, jumps to app"`.

## Don't, in M1
Reprogramming/UDS (M3), app **signature** verification + keys (M4), FreeRTOS/the app proper
(M2), lifecycle/fuse burn (gated, ~M4). The header + digest macro already leave room for these.
