# Design brief — M1: bootloader MVP (FBL boots → verifies → jumps to app)

> **How to use this:** paste as your opening message to Claude Code in the repo root. It
> has `CLAUDE.md` + `docs/architecture/overview.md`. This brief narrows the task to M1.
> First deliverable is a design discussion — do NOT let it jump to implementation.

## Goal

Stand up the **flash bootloader (FBL) MVP** on Node A (Cortex-M4): power on → (ROM verifies
FBL) → FBL reads the **no-init RAM handshake** → FBL **verifies the app image** → **jumps**
to the app (VTOR/MSP). This is the system-partitioning centrepiece. Reprogramming (UDS) is
**M3, out of scope here**; full app-signature verification is **M4** — for M1 a hash/stub
check is acceptable as a placeholder with the real verification deferred.

## Hard constraints (non-negotiable)

1. **FBL stays minimal (ADR-0004/overview §3).** Super-loop or a tiny cooperative tick —
   **no preemptive scheduler, no FreeRTOS** in the bootloader. Small attack surface, small
   code-to-trust. Justify every dependency pulled in.
2. **Shared modules via per-variant config (ADR-0004).** The FBL's CAN/flash/crypto come
   from `shared/` with a **bootloader config** (e.g. CAN polled+minimal). Prefer config data
   over `#ifdef`. Don't fork shared code.
3. **Host-testability (ADR-0001).** Decision logic — handshake parsing, image-validity check,
   "boot vs stay in programming mode" policy, manifest/header parsing — must be host-testable
   under CI behind `shared/hal` interfaces with fakes. The actual jump, VTOR/MSP, flash, and
   ECC priming are target-only behind the port.
4. **MISRA C:2012 (ADR-0003).** Static allocation, fixed-width types, no `<stdio.h>` in
   production. Unity for host tests.
5. **Security is real, not theatre (ADR-0006).** App verification is meaningless without the
   **ROM root of trust** anchoring the FBL — treat ROM secure-boot config as part of M1's
   design even if fuse-burning is deferred. **Never burn fuses without an explicit,
   documented, reviewed step** — fuse operations are irreversible on real silicon.
6. **Comprehension gate (workflow.md).** I must be able to explain the jump and the ECC
   handling in an interview. Explain as you go.

## What I want you to PROPOSE first (before any code)

Design discussion with tradeoffs + at least one alternative for each:

- **Boot decision policy.** The state machine: cold boot vs warm reset; "boot app" vs "stay
  in FBL / programming requested" vs "app invalid → stay in FBL." What inputs drive it
  (handshake region, app-valid check, a programming-request flag).
- **No-init shared RAM handshake.** Layout of the region, the **valid-signature** scheme that
  gates its use, and the **ECC priming** design: cold boot primes once (write to set valid
  ECC) then marks valid; warm/soft reset preserves untouched. *(verify ECC behaviour in TRM)*
  Address what happens on first-ever boot and on corruption.
- **App-validity / image header.** Minimal app header/manifest the FBL reads (magic, version,
  length, CRC/hash, later a signature slot). For M1 a CRC/hash check is enough; design the
  header so M4 can add signature verification without reshaping it.
- **The jump.** VTOR relocation to the app vector table, setting MSP from the app's initial
  SP, branching to the app reset vector; disabling/again-enabling interrupts and any
  peripheral de-init needed for a clean handover. Call out the classic mistakes.
- **The port interface (HAL seam).** Exactly what the FBL core needs from the target port:
  read/prime no-init region, read app header, compute hash/CRC, do-the-jump, enter/exit
  programming mode, reset. Keep minimal and host-fakeable.
- **Memory map + linker.** FBL region vs app region in code flash, the `.noinit` SRAM region,
  vector-table placement. Propose the linker-script structure for both images. *(addresses
  verify in TRM)*
- **ROM secure boot (design only for M1).** How the root of trust anchors the FBL on this
  chip, key-hash storage, lifecycle stages — enough to show the chain is real. Mark
  fuse-burning as a separate, gated, deferred step. *(verify mechanism in TRM)*

## Scope for M1 (keep it minimal)

In scope: FBL super-loop, no-init handshake (+ ECC priming), app-validity check (CRC/hash),
the jump, memory map + linker scripts, ROM-secure-boot **design**, a minimal "app" stub to
jump into so the handover is demonstrable.

**Deliberately deferred** (do NOT build now):
- UDS reprogramming / download sequence — **M3**.
- Full app **signature** verification + key handling — **M4** (design the header to accept it).
- FreeRTOS / the app proper — **M2**.
- SecOC, fault injection, safe-state — later milestones.

## Deliverables from this session (in order)

1. The design discussion above — tradeoffs + alternatives, so I can decide.
2. On agreement: **ADR-0007** (no-init RAM handshake + ECC priming) and **ADR-0008**
   (boot decision policy + jump), in `docs/architecture/decisions/`.
3. The FBL core interfaces + the `shared/hal` port additions (headers, documented).
4. **Failing Unity tests** for the host-testable logic (handshake parse, boot-decision policy,
   app-header/validity check) with a host fake port.
5. Implement the core against the tests; `make test` + `make lint` green. Target-only port
   (jump/VTOR/MSP/flash/ECC) stubbed with clear TODOs until on-board bring-up.
6. A skeleton ModusToolbox layout note for `node_a_gateway/bootloader/` (config, linker,
   src) — actual MTB project generated on-board (board still shipping).

Start with step 1 only. Wait for my agreement before writing ADRs or code.

## Watch-fors (call these out, don't gloss)

- ECC faults on uninitialised `.noinit` reads — the priming pattern must handle first boot.
- A jump that works in debug but not from cold boot (VTOR/MSP/peripheral-state differences).
- Root of trust treated as optional — it isn't; say so.
- Shared-module config drifting into `#ifdef` sprawl — push back toward config data.
