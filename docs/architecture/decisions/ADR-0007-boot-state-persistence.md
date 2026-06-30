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

## M2 extension — the app side of the handshake (D7–D9)

*Added 2026-06-28 for M2 (Node A application). M1 built the **FBL read side** of the
`.noinit` channel; M2 adds the **App write side** and pins the region across both images.
This extends the accepted ADR; it does not revise D1–D6.*

### D7. App-side write path — App is a producer of one field only

The App writes exactly one thing to `.noinit`: a programming-request. The encode is **pure
logic in `shared/boot`** (mirror of the FBL's validate), the side effects are two App ports:

```c
/* shared/boot — pure, host-testable (mirrors the FBL read/validate). */
void boot_handshake_encode(fbl_handshake_t *out, fbl_boot_mode_t mode);
                  /* fills magic, version, mode, crc32 over the preceding bytes */

/* App request flow (target glue is two ports; host fakes back both). */
void App_request_reprogram(void)
{
    fbl_handshake_t h;
    boot_handshake_encode(&h, FBL_PROGRAMMING_REQUESTED);
    *app_port_noinit_region() = h;   /* store to the pinned .noinit (D8)        */
    __DSB();                         /* ensure the write lands before reset     */
    app_port_system_reset();         /* NVIC_SystemReset                        */
}
```

App port hooks introduced: `app_port_noinit_region` (returns the pinned `.noinit` pointer)
and `app_port_system_reset`. Host fakes back them with a RAM buffer and a "reset requested"
flag, so the request path unit-tests without hardware. The App **never** touches BREG (D1).

**On-board trigger for M2:** a simple debug trigger (the kit **user button**) calls
`App_request_reprogram()`. Real UDS-driven reprogramming is **M3**; M2 only exercises the
mechanism, and the trigger is intentionally non-CAN so seam 3 is independent of seam 2.

### D8. Linker pinning of `.noinit` across **both** images (M2-3)

M1 left a latent cross-image hazard: the FBL pinned `.noinit` at `0x0801_FF00` (top−256) —
**inside the BSP's reserved top-2 KB**, and with the FBL's whole SRAM region based at
`0x0800_0000`, i.e. **in the CM0+ RAM tenant** — while the app's stock BSP linker left
`.noinit` **floating** in its `ram` region. The two images did not agree on the address.

The M2 fix — one region, one definition, honoured by both:

```
0x0800_0000  CM0+ RAM tenant (32 KB)          ── both images AVOID
0x0800_8000  CM4 RAM (.data/.bss/stack/        ── app: ~93.75 KB usable
             FreeRTOS static)
0x0801_F700  .noinit handshake (256 B)         ── PINNED, NOLOAD, both linkers
0x0801_F800  reserved "system use" (2 KB)      ── both images AVOID (BSP rule)
0x0802_0000  top of SRAM
```

- **`.noinit` base = `0x0801_F700`, size `0x100`** — below the reserved 2 KB (legal) and
  above the CM0+ tenant (safe). The handshake struct is only 16 B; the 256 B is alignment
  headroom and room for future handshake fields — **not** an App register block (the App's
  cross-reset state lives in BREG, D6, not in `.noinit`).
- **Single source of truth:** a shared fragment `shared/boot/linker/noinit.ld`
  (`_noinit_start = 0x0801F700; _noinit_size = 0x100;`) is `INCLUDE`d by **both** linkers,
  which shrink CM4 RAM `LENGTH` to end at `_noinit_start`. A future address mismatch becomes
  structurally impossible.
- **The handshake gets its OWN section — not the shared `.noinit` (silicon-verified).** The
  general `.noinit` section is **not ours alone**: the BSP `system_psoc6` plus `cyhal`/`cy_syslib`
  contribute to it (≈`0x5A0`+ bytes), and they land *first*, so pinning the whole `.noinit`
  output section put our handshake at an **offset** (observed `0x0801_FCA0`, inside the reserved
  2 KB) — matching across images only by coincidence of identical `.noinit` layouts, which a BSP
  or lib change would silently break. Fix: the handshake lives in a **dedicated `.fbl_handshake`
  section** pinned at `_noinit_start`; the general `.noinit` floats back in `ram` (`> ram`, NOLOAD)
  for the BSP/libs. The `ASSERT(ADDR(.fbl_handshake) == _noinit_start)` guards it. The handshake
  object (`g_handshake` in both `app_port_target.c` and `port_noinit.c`) carries
  `__attribute__((section(".fbl_handshake")))`.
- **Guard against BSP regeneration:** `app_cm4.ld` is a ModusToolbox-generated file, so a BSP
  re-generation could silently revert the `INCLUDE` + length edit and reintroduce the very
  M2-3 hazard this fixes. Mitigation: the app **forks `app_cm4.ld` into a repo-owned linker**
  (as the FBL already owns `fbl.ld`) and points the build at it, rather than editing the
  generated copy in place. The fork is a tracked file; regeneration cannot touch it. A
  link-time `ASSERT` that `.noinit` sits at `_noinit_start` is added as a second guard.
- **Also corrected:** the FBL `SRAM` origin moves from `0x0800_0000` to `0x0800_8000` so the
  FBL's own `.data/.bss/stack` stop living in the CM0+ tenant. This changes the FBL image, so
  **M1 boot is re-verified on board** (it is M2 bring-up seam 1 regardless).
- The App's C-startup must **not** zero `.noinit` (it is `NOLOAD` and excluded from the
  startup zero-table) — verified when the app linker change lands.

### D9. ECC-priming ownership and the M2-5 counter interaction (reaffirmed)

- **The FBL is the sole ECC primer** (D3). It always runs first and primes on its prime-cause
  path; on a read-cause the region was retained ECC-valid from a prior prime. So **the App
  consumes a primed, ECC-valid region and never primes** — its write in D7 is an ordinary
  store to valid ECC SRAM, with no first-read fault and no prime race.
- **M2-5 is the first silicon exercise of the B2 rule (D4).** App writes
  `FBL_PROGRAMMING_REQUESTED` → software reset → FBL classifies *software reset* → `.noinit`
  action = **READ** → valid `magic`/`crc32` + `mode == PROGRAMMING_REQUESTED` ⇒ **programming
  mode** (ADR-0008 D1) **and counter CLEAR → 0** (D4: "sw-reset *with* programming-request").
  The required host test (M2 deliverable): *App requests reprogram N× in a row ⇒ programming
  mode every time, boot-loop fallback (counter > N) never trips.*

**Host/target split additions:** host-testable — `boot_handshake_encode` and the M2-5
sequence over the existing classify + counter rule; target-only — `app_port_noinit_region`,
`app_port_system_reset`, and the two-linker `.noinit` pinning.

## Review history

Design-reviewed before implementation (`docs/review/ADR-0007-0008-review.md`). Findings
actioned here: **B2** — D4 made explicit (sw-reset + `.noinit` programming-request ⇒ clear;
hibernate-wake classified independently so it never increments); **B6** — backup-domain
register map (D6) reserves FBL and App partitions up front. Counter increment is now
single-sourced in D4 (the duplicate in ADR-0008 was removed — B1).

The **M2 extension (D7–D9)** was reviewed in `docs/review/ADR-0010-0011-review.md`. Findings
actioned: **2** — fork `app_cm4.ld` into a repo-owned linker + a link-time `ASSERT`, so a BSP
regeneration can't silently un-pin `.noinit` (D8); **6** — reworded the 256 B rationale (the
App's cross-reset state lives in BREG per D6, not `.noinit`) (D8); **S4** (silicon) — the
handshake gets its **own** `.fbl_handshake` section pinned at `_noinit_start`, not the shared
`.noinit` catch-all (which the BSP/libraries populate first, pushing the handshake to a floating
offset `0x0801_FCA0`) (D8).
