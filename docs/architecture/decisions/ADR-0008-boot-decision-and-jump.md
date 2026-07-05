# ADR-0008: Boot-decision policy, knock window, and the app jump

**Status:** accepted · **Date:** 2026-06-21

## Context

Given the persisted boot state from ADR-0007, the M1 FBL must decide, once per reset,
whether to **stay in the bootloader** (programming mode), **jump to the application**, or
**stay as a fail-safe** — and then perform a clean Cortex-M handover. The FBL is kept
deliberately minimal (ADR-0004), security must be real rather than theatre (ADR-0006), and
the decision logic must be host-testable (ADR-0001) with only the jump itself target-only.

Reprogramming (UDS) is **M3** and full signature verification is **M4**; this ADR designs
the M1 MVP so both slot in without reshaping.

## Decisions

### D1. The boot-decision tree

Evaluated once per reset, after ADR-0007 has classified the reset cause and reconciled the
`.noinit` region and the BREG counter:

```
1. programming-requested? (.noinit)            → FBL programming mode
2. boot-attempt counter > N? (BREG)            → FBL programming mode   (boot-loop fallback)
3. power-on AND lifecycle < SECURE?            → KNOCK WINDOW: knock → FBL ; timeout → continue
4. app image valid? (digest, D3)               → JUMP (D4)
5. else (invalid / blank app)                  → FBL programming mode   (fail-safe)
```

The boot-loop counter is **maintained in exactly one place — ADR-0007 D4, at FBL entry by
reset cause.** The decision here only *reads* it (step 2); step 4 does **not** touch it.
(An earlier draft incremented at the jump as well, which double-counted — B1.)

**Ordering rationale.** Programming-request is first so a *valid* app can still ask to be
reflashed (the essence of a field-updatable ECU). The boot-loop fallback (2) precedes the
jump so a known-bad app is caught before we run it again. The knock window (3) precedes the
app check so a tool can grab the bootloader *before* a valid app boots. The fail-safe (5) —
**invalid/blank app ⇒ stay in FBL, never jump** — is the single most important safety
property: a half-flashed app must never execute.

### D2. Knock window — lifecycle-gated developer/recovery entry

A bounded wait, opened **only on a power-on reset AND when the device lifecycle is below
SECURE**, during which a tool may "knock" to keep the FBL resident.

- **M1 contact = a fixed knock frame on the diagnostic CAN ID** (`fbl_port_tool_contact`
  polls for it). In **M3** the same hook becomes a real UDS DiagnosticSessionControl
  request — the core does not change.
- **Default `T_knock` = 2 s** (configurable). It is paid on every *dev* cold boot and on
  *zero* production boots.

**The lifecycle gate is a security control, not a convenience flag.** The knock path is a
development/recovery backdoor; it must close itself once the device is locked into the
SECURE lifecycle, or an attacker could hold a production ECU in the bootloader from the
bus. So: NORMAL / TRANSITION_TO_SECURE ⇒ window may open on cold boot; **SECURE ⇒ never**.
In SECURE, the only routes into programming are an app-initiated `.noinit` request or an
*authenticated* diagnostic session (M3 security access) — the same gate the knock path
loses. *(Exact lifecycle stage names/values verify in TRM.)*

This is a single reusable dwell primitive (`fbl_dwell_for_tool(window_ms)`), pure logic
over `fbl_port_now_ms` + `fbl_port_tool_contact`, host-tested with a fake clock and a
scripted contact timeline.

### D3. App-validity / image header

**The digest must not cover itself (B4).** An integrity field cannot be inside the range it
protects, so the image is laid out as a **descriptive header** (covered) plus an
**integrity trailer** (excluded):

```
app_base               : app vector table                         ┐
app_base + VECT_SIZE   : descriptive header                       │  covered by the
                           magic u32, hdr_version u16,             │  digest:
                           hdr_size u16, image_len u32             │  [app_base ..
... app code ...       :                                          ┘   app_base+image_len)
app_base + image_len   : integrity trailer  ── EXCLUDED ──
                           M1:  digest (CRC32, 4 B)
                           M4:  hash[32] + signature[] + key_id u32
```

`digest = D([app_base .. app_base+image_len))` — this protects the vector table and the
descriptive header but stops exactly at the trailer, so it never covers itself. The FBL
reads `image_len` from the fixed-offset header first (no chicken-and-egg), computes the
digest over the body, then reads and compares the trailer at `app_base + image_len`. **M4
inherits the layout unchanged:** the hash is over the same body range, the signature is over
the hash, and both live in the (still-excluded) trailer. Lock this layout now.

**M1 check:** recompute the digest over `[app_base .. app_base+image_len)`, compare to the
trailer, **plus** a sanity check on the app vector table before trusting it (B8):

- initial MSP is **within SRAM bounds and 8-byte aligned** (AAPCS), not merely "into SRAM";
- the reset vector points **into app flash** with the **Thumb bit set**.

**Digest is macro-selectable**, so M1 ships lean CRC32 and M4 flips one switch — the
covered range and verify flow are identical, only algorithm and size change:

```c
#define FBL_DIGEST_CRC32 1
#define FBL_DIGEST_SHA256 2
#ifndef FBL_DIGEST_ALGO
#define FBL_DIGEST_ALGO FBL_DIGEST_CRC32   /* M1 default; M4 → SHA256 */
#endif
```

**Honesty note:** CRC32 proves the image is **complete and uncorrupted** (the real M1 risk
— an interrupted flash); it is **not** authenticity. The reserved signature slot is where
M4 adds it, and M4 authenticity = hash **+ signature-over-the-hash** (via `shared/crypto`),
not the hash alone.

### D4. The jump (target-only)

1. App verified (D3).
2. Read `app_MSP = *(app_base)`, `app_reset = *(app_base+4)`; sanity-check both.
3. **De-init:** `__disable_irq()`, stop SysTick, disable + **clear-pending** all NVIC IRQs
   (`ICER`/`ICPR`), return FBL-touched peripherals (CAN, clocks) toward reset state.
4. `SCB->VTOR = app_base`.
5. `__set_MSP(app_MSP)`.
6. `__DSB(); __ISB();`
7. Branch to `app_reset`; let the app's startup re-enable interrupts.

**Classic mistakes this guards against** (the "works in debug, not from cold boot" trap):

- Forgetting **MSP** ⇒ app runs on the FBL stack ⇒ corruption. Forgetting **VTOR** ⇒ app
  interrupts vector into FBL handlers.
- A **live peripheral/IRQ** (SysTick, CAN RX) firing mid-handover into a half-set-up table
  ⇒ fault. Disable *and clear pending*.
- A **debugger** resets peripherals/clocks and may set VTOR for you; cold boot does none of
  that — always validate from cold boot.
- **Using the stack between `__set_MSP` and the branch** is undefined; the final two steps
  live in a small **naked/asm helper** `fbl_port_jump_to_app(msp, reset)` with nothing
  stack-dependent in between — not the fragile "set MSP in C then call a function pointer".
  That helper is inline assembly, so it carries a **MISRA Dir 1.1 / Rule 4.3 deviation**
  (assembly encapsulated in a dedicated port function) — the row is added to
  `docs/coding-standard.md` when the port code lands, same as the scheduler port asm (B7).

**FBL → app handover contract (B3/B9).** Decoupling — the app owning its own IRQ/clock/
peripheral re-init — is the deliberate design choice (identical behaviour whether the app is
entered from the FBL, a debugger, or a future loader). For that to be safe, the FBL must
hand over in a quiescent state, so the boundary is a written guarantee, not luck:

> **The FBL guarantees, before branching:** all IRQ sources **disabled and pending cleared**
> — including the SysTick pending bit via `SCB->ICSR` (`PENDSTCLR`) — `VTOR` and `MSP` set to
> the app's values, and `CONTROL` in its reset state (MSP, privileged). Clocks are handed
> off at reset state.
> **The app is responsible** for re-enabling interrupts and re-initialising clocks and
> peripherals as part of its own startup.

Step 3 already performs the disable + clear; this just makes the contract explicit so the
handover is correct by design rather than by the app happening to re-init.

### D5. Root of trust and lifecycle (design for M1, defer the burn)

The chain is **ROM verifies FBL → FBL verifies app → app** (ADR-0006). For M1:

- Design and document the chain and what must be provisioned (public-key hash, TOC2
  entries, lifecycle target) *(verify mechanism in TRM)*.
- Build the **FBL-verifies-app structure** now (CRC32 placeholder → M4 signature).
- **Develop in the open (pre-SECURE) lifecycle** so the board stays reflashable; the knock
  window (D2) is the convenience this buys.
- **Lifecycle advancement / fuse provisioning is a separate, gated, reviewed, irreversible
  step** — never casual, almost certainly alongside M4. Until then, **the system is not
  actually secure**, and the design says so plainly.

## Host/target split (ADR-0001)

| Host-testable core (`shared/boot`) | Target-only (FBL port) |
|---|---|
| The whole decision tree (D1) | The VTOR/MSP/branch jump (D4, asm helper) |
| Knock dwell logic (D2) | CAN poll for the knock frame; lifecycle read |
| Header parse + digest check + vector sanity (D3) | Reading flash (memory-mapped); HW-CRC (optional) |

The entire tree is pure over port inputs `(reset_cause, lifecycle, tool-contact timeline,
app-digest-valid, BREG counter)`, so one Unity suite drives it end to end —
e.g. "cold + SECURE + valid app ⇒ jump, no knock latency", "cold + NORMAL + valid app +
no-knock ⇒ jump after T_knock", "warm + counter>N ⇒ programming mode".

## Consequences

- (+) Layered recovery with nothing over-built: corrupt app → digest fail-safe (5);
  corrupt `.noinit` → reinit (ADR-0007 D5); valid-but-bad app → BREG counter (2);
  deliberate dev entry → knock (3).
- (+) The lifecycle gate makes the dev backdoor self-closing in production.
- (+) Header + digest macro make the M4 signature upgrade a one-switch change.
- (−) The knock window adds `T_knock` latency to every pre-SECURE cold boot (zero in
  production) — an accepted dev cost.
- (−) The jump and lifecycle/secure-boot specifics depend on TRM details to be confirmed on
  the board.

## Alternatives considered

- **Handshake-only entry (no knock window):** simpler, but no app-independent way to grab a
  bricked-but-valid app's bootloader; the knock window (lifecycle-gated) is the recovery
  path. Kept.
- **A/B banking with rollback:** the production-grade answer to a bad app, but doubles flash
  and adds bank logic. Deferred; the header does not preclude it.
- **"Set MSP in C, call a function pointer" jump:** works only if the compiler inserts no
  stack use before the call — fragile. Rejected in favour of the asm helper.
- **Enable secure boot in M1:** more "real", but risks bricking a dev board via an
  irreversible lifecycle change. Rejected for M1.

## Review history

Design-reviewed before implementation (`docs/review/ADR-0007-0008-review.md`). Findings
actioned here: **B1** — removed the duplicate counter increment in D1 step 4 (single-sourced
in ADR-0007 D4); **B4** — D3 reworked into a covered descriptive header + an excluded
integrity trailer so the digest never covers itself (M4 inherits the layout); **B3/B9** —
added the FBL→app handover contract to D4; **B7** — noted the asm-helper MISRA deviation;
**B8** — extended the vector-table sanity check (MSP in SRAM bounds + 8-byte aligned).

The FBL's M3 Seam 3 bring-up (the knock window running for real) surfaced two further
findings, this time in the boot-time environment the decision tree runs in, not the decision
logic itself (host-tested and unchanged):

- **S1** (silicon) — `fbl_port_lifecycle()` (`port_security.c`) was still the M1 stub,
  hardcoded to `FBL_LC_SECURE` — a correct *safe default* while nothing needed the knock
  window to actually open, but wrong for this stage: per D5's own design intent ("develop in
  the open (pre-SECURE) lifecycle... the knock window is the convenience this buys"), a dev
  board should read as pre-SECURE through M1–M4, and this board has never been
  lifecycle-provisioned. Replaced with a real read of `CPUSS_PROTECTION.STATE` (`0x402020C4`,
  confirmed against `docs/references/CPUSS_Protection_Secure_State.png`), mapping the
  register's five raw states (`UNKNOWN`/`VIRGIN`/`NORMAL`/`SECURE`/`DEAD`) onto
  `fbl_lifecycle_t`'s four — `VIRGIN` → `NORMAL` (unprovisioned is at least as open),
  `UNKNOWN`/`DEAD` → `SECURE` (ambiguous or terminal, deny the knock window; same prime-bias
  safe-default direction as ADR-0007 D3).
- **S2** (silicon) — the vendor startup leaves `PRIMASK` set (interrupts globally masked)
  after `SystemInit()`/`cybsp_init()`, expecting whatever runs next to turn them back on; an
  RTOS scheduler start does this for the App for free (ADR-0010), but the FBL is bare-metal
  (ADR-0004: no RTOS) and nothing ever called `__enable_irq()`. This silently broke
  `fbl_port_now_ms()`: SysTick's counter ran correctly in hardware (confirmed via
  `SysTick->CTRL`/`->VAL`), but its interrupt was never taken, so `s_tick_ms` never
  incremented — `fbl_dwell_for_tool()`'s timeout branch could then only ever be escaped by a
  knock, never by elapsed time (the dwell hung indefinitely with no CAN traffic and only
  appeared to work once traffic started). Fixed with an explicit `__enable_irq()` in
  `fbl_main()`, right after `cybsp_init()`.

  **Retroactive impact on ADR-0013's S1:** this same `PRIMASK` bug was present through M3
  Seam 2 (ISO-TP) bring-up too, meaning `isotp.c`'s `N_Cr` timeout (ADR-0013 D3) was never
  actually functional on hardware at the time that seam was verified — the timing
  degenerates harmlessly to `now_ms - last_activity == 0` when `now_ms` never advances,
  which happens not to matter for a prompt, successful exchange. Seam 2's success did not
  exercise the timeout path; an interrupted-transfer scenario before this fix would not have
  timed out correctly. Both `N_Cr` and the knock dwell now share the same real clock, fixed
  once here.
