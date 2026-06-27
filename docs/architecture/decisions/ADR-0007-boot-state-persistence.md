# ADR-0007: Boot-state persistence — `.noinit` shared region + backup registers, reset-cause classification

**Status:** accepted · **Date:** 2026-06-21

## Context

The M1 flash bootloader (FBL) on Node A must carry two kinds of state across a reset:

1. **An App→FBL programming request.** The application's diagnostics writes "please stay
   in the bootloader" and triggers a software reset; the FBL must read that intent on the
   next boot. This is a **shared channel** between two images.
2. **The FBL's own boot-loop detection.** To recover from a *valid-but-misbehaving* app
   (passes the image check, then crash-loops), the FBL keeps a private counter. The app
   must not be able to see or tamper with it.

Three hardware realities shape the design (all *(verify in TRM)*):

- **SRAM is ECC-protected.** Reading a location never written since power-on returns
  garbage ECC and can raise a fault — so we cannot blindly read an "uninitialised"
  handshake region.
- **Hibernate powers down the SRAM domain.** Only the always-on **backup domain**
  (backup registers / RTC) survives hibernate; `.noinit` SRAM does not.
- **Reset causes differ** in what they preserve (power-on vs software vs watchdog vs
  debug vs hibernate-wake).

All decision logic here must be host-testable (ADR-0001) and MISRA-clean (ADR-0003);
the chip access sits behind the FBL port (see ADR-0008 §port).

## Decisions

### D1. Two storage tiers with distinct ownership

| | `.noinit` shared SRAM region | Backup registers (BREG) |
|---|---|---|
| **Owner / visibility** | App **and** FBL — the handshake channel | **FBL only** — the App never reads/writes it |
| **Holds** | programming-request, `magic`, `crc32` | boot-attempt counter, `magic` |
| Survives warm reset (sw/wdt/debug) | yes | yes |
| Survives **hibernate** | **no** (re-primed on wake) | **yes** (backup domain stays powered) |
| Survives power-on | no | no |
| ECC priming needed | **yes** (this is the gotcha below) | no (not ECC-SRAM) |
| Purpose | App→FBL "reprogram me" | FBL's autonomous boot-loop fallback |

The ownership boundary is deliberate: the programming request is **App-originated**, so it
must live where the App can write it (`.noinit`); the boot-loop counter is **FBL-owned**
recovery state, so it lives where the App cannot reach it (BREG).

### D2. Reset-cause classification

The port returns a raw cause; the **classification is pure logic in the core** (one
host-tested table). Each cause drives three independent behaviours:

| Reset cause | `.noinit` (SRAM lost?) | Knock-window eligible? (ADR-0008) | BREG counter |
|---|---|---|---|
| **Power-on / brown-out** | lost → **prime** | yes (if lifecycle < SECURE) | **clear → 0** |
| **Software reset** | retained → **read** | no | **increment** |
| **Watchdog reset** | retained → **read** | no | **increment** |
| **Debug reset** | retained → **read** | no | unchanged |
| **Hibernate-wake** | lost → **prime** | **no** (fast wake) | unchanged |
| **Other / unknown / multiple** | **prime** (safe default) | no | treat as power-on path |

Two implementation notes — **now confirmed against the TRM** (`RES_CAUSE` @ `0x4026_1800`,
`docs/references/Reg_ResetCause.png`); the PDL `Cy_SysLib_GetResetReason()` exposes these:

- **POR is bit 30 (`RESET_PORVDDD`), not `cause == 0`.** An earlier draft assumed a POR
  clears the register to 0; on this part the register's **default is `0x4000_0000`** (bit 30
  set), so POR is detected by that bit, together with brown-out (`BOD*`) and external reset
  (`XRES`/`PXRES`) as the "came up cold" group. (Verified by silicon — the assumption was
  wrong; see `port_reset.c`.)
- **Clear the cause register after reading** (`Cy_SysLib_ClearResetReason`). Decode with the
  cold group taking priority (prime-bias), and check **hibernate-wake first** — the
  low-voltage cause bits re-assert on wakeup on this part, so it must not be misread as POR.

### D3. ECC priming and the prime-bias safety rule

- **Prime-cause** (power-on / hibernate / unknown): the region's ECC is invalid or gone.
  **Write the region** to establish valid ECC, then initialise to defaults
  (`mode = boot-app`) and set `magic` + `crc32`.
- **Read-cause** (software / watchdog / debug): the region is retained and ECC-valid →
  **read and validate** (`magic` + `crc32`).

The two misclassification directions are **not symmetric**, which fixes the safe default:

- **Classify cold-when-actually-warm** → we prime over good data → lose the handshake,
  fall back to boot-app. *Safe.*
- **Classify warm-when-actually-cold** → we *read* uninitialised ECC SRAM → **fault.**
  *Dangerous.*

Therefore classification is **prime-biased**: only causes we are certain retain SRAM map
to "read"; everything ambiguous maps to "prime." The `magic`+`crc32` check is a second
gate behind the classification — a region we called "read" that fails validation is
reinitialised rather than trusted.

### D4. BREG boot-loop counter rule

The FBL maintains a private counter in BREG, updated at FBL entry **after** reset-cause
classification (D2) **and** after the `.noinit` region has been reconciled (D3) — the
ordering matters, because the software-reset case below has to inspect the `.noinit`
programming-request:

| Reset cause | Counter action |
|---|---|
| Power-on | **clear → 0** (a power cycle is a clean start) |
| Software reset **with** `.noinit` programming-request | **clear → 0** (deliberate reflash) |
| Software reset **without** programming-request | **increment** |
| Watchdog reset | **increment** |
| Debug reset | unchanged |
| Hibernate-wake | unchanged — **classified independently** (never increments) |

Trip condition (consumed by the boot decision, ADR-0008): **`counter > N`** (default
`N = 3`) ⇒ the app is crash-looping ⇒ do not jump; stay in the FBL.

Two precision points the design depends on:

- **A deliberate reflash is not a crash (B2).** The one legitimate bare software reset is
  the diagnostics-triggered "reprogram me", which the FBL recognises by the `.noinit`
  programming-request. That case **clears** the counter — a freshly flashed app deserves a
  clean `N` attempts, not the previous app's crash count. (This subsumes the earlier "M3:
  clear on successful reprogram" note.) A software reset with **no** programming-request is
  a genuine anomaly and is counted.
- **Hibernate-wake is classified ahead of / independently from the generic reset-cause
  path (B2)**, so a low-power wake can never fall through to "software reset ⇒ increment".
  An ECU that sleeps and wakes all day must not slowly climb the counter and falsely fall
  back to the FBL.

**Why this rule needs no app-health flag.** The counter climbs *only* on unmarked crash
resets (software/watchdog). A healthy app kicks the watchdog and keeps running, so it never
increments the counter; a power cycle or a deliberate reflash zeroes it. An earlier draft
added an "app-confirmed" flag in `.noinit` to clear the counter — this rule makes it
unnecessary, which also keeps the `.noinit` channel minimal (programming-request only).
BREG self-validates via its own `magic` (no ECC, always readable), so a power-on with a
cleared backup domain reads as "invalid → counter 0".

### D5. Corrupt `.noinit` on a read-cause → reinit to default

If a read-cause region fails `magic`/`crc32` (a flipped bit), **reinitialise it to the
boot-app default and continue** to the boot decision. We deliberately do **not** add a
recovery wait/dwell here — the downstream layers already cover the real risks (the image
digest check guards a bad app; the BREG counter guards a crash-loop). A dwell would be
latency and code for a case those layers already handle.

### D6. Backup-domain register map — reserve FBL and App partitions now (B6)

The backup registers are a **scarce, fixed, shared** resource, and the FBL's block holds
the one piece of state that must never be wrong. So the register map is **partitioned and
reserved up front**, even though only the FBL block is implemented in M1:

| Partition | Owner | Contents | Status |
|---|---|---|---|
| `BREG[0..k_fbl]` | **FBL** | boot-loop counter + `magic` | M1 |
| `BREG[k_fbl+1 ..]` | **App** | future wake-context (wake reason, resume state, graceful-shutdown flag) | **reserved**, not M1 |

The reservation exists because of the hibernate path: on wake, the app comes back through a
reset with zeroed `.bss` and a **lost `.noinit`**, so any state it wants to carry across
hibernate must live in the backup domain — and it cannot reuse `.noinit` (gone) or the
FBL's counter block (walled off by D1; letting the app touch it erodes the tamper-resistance
that block exists to provide). Retrofitting an app partition later would mean renumbering
the FBL's safety-critical registers and re-verifying them, so we fix the boundary now.
*(Exact register count and whether this part offers retained backup SRAM instead — verify
in TRM; if retained backup SRAM exists, the App partition can move there and the BREG block
stays FBL-only.)*

## Host/target split (ADR-0001)

| Host-testable core (`shared/boot`) | Target-only (FBL port) |
|---|---|
| Reset-cause → classification table (D2) | Read/clear SRSS reset-cause register |
| Prime-vs-read decision, prime-bias (D3) | ECC-priming write to the `.noinit` region |
| `.noinit` parse/validate/init + CRC | BREG read/write (backup-domain access) |
| BREG counter rule (D4), `magic` checks | — |

Port hooks introduced: `fbl_port_reset_cause`, `fbl_port_clear_reset_cause`,
`fbl_port_noinit_region`, `fbl_port_prime_noinit`, `fbl_port_backup_read`,
`fbl_port_backup_write`. Host fakes back these with a settable cause, a RAM buffer, and a
register array, so the whole table and counter rule unit-test without hardware.

## Consequences

- (+) Hibernate is covered correctly: classified as prime (no ECC fault), and the desired
  wake behaviour (boot the app) is just the primed default — nothing needs to *survive*
  hibernate.
- (+) Clean App/FBL boundary: the App's only persistence surface is the `.noinit` channel;
  the recovery counter is tamper-proof in BREG.
- (+) The prime-bias rule makes an unforeseen reset cause safe by construction.
- (−) Two stores to keep coherent (addresses, magics) — mitigated by defining each once.
- (−) Several mechanisms ride on TRM-specific behaviour that must be confirmed on silicon
  (see below); the host tests prove the *logic*, not the chip.

## Alternatives considered

- **`.noinit` SRAM only (no BREG):** the programming request works, but boot-loop state is
  lost on hibernate and the counter would be App-visible/tamperable. Rejected.
- **BREG only (no `.noinit`):** the App cannot be allowed to write FBL backup registers, so
  there is no shared channel for the programming request. Rejected — breaks D1's boundary.
- **Reset-cause-blind, ECC-fault-tolerant read:** configure ECC errors as a readable status
  and probe the region instead of classifying. More robust to reset-cause quirks but
  TRM-specific and more moving parts; kept in reserve, not the default.

## To verify in the TRM / app notes

- ~~Reset-cause register(s) and bit semantics; how POR/BOD are reported; the
  `cause == 0 ⇒ POR` assumption.~~ **Resolved:** `RES_CAUSE` bit 30 = POR (`RESET_PORVDDD`),
  not `cause == 0` — see D2 and `port_reset.c`. *(Hibernate-wake reporting on this part still
  to confirm — not exercised in M1.)*
- Backup-register count and retention semantics across hibernate, warm reset, and power-on
  (and whether the backup domain needs a separate supply on this board).
- ECC behaviour on uninitialised SRAM reads (trap vs. status) and ECC word granularity for
  the priming write.

## Review history

Design-reviewed before implementation (`docs/review/ADR-0007-0008-review.md`). Findings
actioned here: **B2** — D4 made explicit (sw-reset + `.noinit` programming-request ⇒ clear;
hibernate-wake classified independently so it never increments); **B6** — backup-domain
register map (D6) reserves FBL and App partitions up front. Counter increment is now
single-sourced in D4 (the duplicate in ADR-0008 was removed — B1).
