# Design review — ADR-0007 (boot-state persistence) & ADR-0008 (boot decision + jump)

**Reviewed:** 2026-06-21 · **Status:** discussed — findings actioned into ADR-0007/0008 (see each ADR's Review history).
**Verdict:** strong, mature design — more so than the scheduler ADRs in places. The
prime-bias safety argument (ADR-0007 D3) and the layered fail-safe ordering (ADR-0008 D1)
are exactly the failure-mode-first thinking the architect role wants, and the honesty notes
(CRC32 ≠ authenticity; "not actually secure until the burn") are the right maturity. Findings
below; **B2 and B3 were discussed and withdrawn/reframed** (see notes — the author's hardware
reasoning was correct), **B6 downgraded to a planning note.**

---

## Must-fix

### B1 — Boot-loop counter is incremented in two places, two different ways
**Severity:** high (internal contradiction → double-count / wrong trip).

ADR-0007 D4 increments the counter **at FBL entry, by reset cause** (sw/wdt → increment,
POR → clear). ADR-0008 D1 step 4 **also** increments — "counter++ (unless hibernate-wake)" —
**at the jump decision**. For a valid app after a software reset, both fire → +2 per boot.
The two models aren't interchangeable: the reset-cause model needs no app-side clear; the
increment-before-jump model *requires* an app-confirmed clear (which ADR-0007 D4 deliberately
removed), or it trips after N healthy boots.

**Fix:** keep the reset-cause model (ADR-0007 D4); **delete the step-4 increment** in
ADR-0008 — step 4 just jumps.

---

## Important

### B4 — The digest cannot cover itself; define the covered range precisely
**Severity:** medium. **Status: accepted by author.**

D3 says the digest covers `[app_base .. app_base+image_len)` while the header (which holds the
digest field) sits just after the app vector table — inside the covered range, which is
impossible to compute consistently. Define the covered range to **exclude the integrity
fields** (digest in M1; hash + signature + key_id in M4). Cleanest: integrity fields in a
header/trailer slot outside the covered range; M4 signature is over the hash, hash covers
everything except the signature slot. Lock this layout now — M4 inherits it.

---

## Discussed and withdrawn / reframed

### B2 — (withdrawn) "software reset = crash" over-counts intentional resets
**Original concern:** healthy apps doing bare software resets would climb the counter and
trip the boot-loop fallback.

**Why withdrawn (author's reasoning, correct):** intentional resets are not bare software
resets in this design. The only legitimate software-reset case is the diagnostic-triggered
"reprogram me," which the FBL disambiguates via the `.noinit` programming pattern. Config /
init-shutdown flows terminate in **hibernate/low-power**, so they return as **hibernate-wake**
(already classified counter-unchanged in ADR-0007 D4). A bare, unmarked software reset really
is an anomaly worth counting.

**Action (small, to close the residual gap):** make the rule explicit in ADR-0007 D4 rather
than implicit —
- software reset **with** the `.noinit` programming pattern ⇒ **clear** the BREG counter
  (deliberate reflash is clean intent, not a crash);
- software reset **without** it ⇒ increment.
And ensure the FBL classifies **hibernate-wake ahead of / independent from** the generic
reset-cause path, so a wake never increments. (D4 currently states "software reset ⇒
increment" unconditionally; add the branch.)

### B3 — (withdrawn) interrupt state across the jump
**Original concern:** FBL `__disable_irq()` then relying on "app startup re-enables" is
fragile because standard CMSIS startup doesn't touch PRIMASK.

**Why withdrawn (author's reasoning, correct):** the app owning its own IRQ/clock/peripheral
re-init is the *better* architectural principle — less coupling, identical behaviour whether
entered from the FBL, a debugger, or a future bootloader. "App re-inits/enables IRQ" is a
deliberate design choice, not a bug. PRIMASK left set is then cosmetic (the app clears it).

**Surviving point (reframed) — document the handover contract.** Decoupling is a two-sided
contract. For "the app re-enables interrupts" to be safe, the FBL must hand over in a state
where nothing fires before the app re-inits. State it explicitly:
> **FBL handover contract:** FBL guarantees all IRQ sources **disabled + pending cleared**
> (incl. SysTick pending via `ICSR`), and `VTOR`/`MSP` set, `CONTROL` in reset state. The
> **app** is responsible for re-enabling interrupts and re-initialising clocks/peripherals.

D4 step 3 already does the disable+clear; this just makes the boundary a written guarantee so
B3 is satisfied by design rather than by luck.

---

## Planning note (you may close)

### B6 — Reserve App-owned backup-domain storage in the register map now
**Severity:** low / planning. **Status: author to decide.**

Forward-looking, for the planned sleep/wake-from-hibernate path. On wake (a reset back through
the FBL into an app with zeroed `.bss` and **lost `.noinit`**), any wake-context the app wants
back (wake reason, resume state, graceful-shutdown flag, diagnostics counters) must have lived
in the **backup domain** — the only thing that survives hibernate. It can't reuse:
- `.noinit` — lost across hibernate (the whole reason it's "primed" on wake);
- the FBL's BREG counter — D1 deliberately walls it off from the app; letting the app write it
  erodes the tamper-resistance D1 exists to provide.

So the app needs a **few backup registers of its own, distinct from the FBL's block**. This is
not an M1 task — the ask is only to **reserve an app partition when laying out the backup-domain
register map now**, since it's a fixed/scarce shared resource and retrofitting later means
renumbering the FBL's registers and re-verifying the one piece of state that must never be wrong.

**May be closed if:** the eventual design keeps *all* wake-context inside the FBL (FBL owns the
wake reason, passes it fresh to the app each boot, app persists nothing across hibernate); or if
this part has retained backup SRAM rather than a scarce handful of backup registers. Decide the
ownership of the backup domain deliberately, then close or keep.

---

## Polish

- **B7** — the naked/asm jump helper is inline asm → MISRA Dir 1.1 / Rule 4.3 (encapsulation);
  add a deviation row (same as the scheduler port asm).
- **B8** — vector-table sanity (D3) could also check the initial MSP is within SRAM bounds and
  8-byte aligned, not just "into SRAM."
- **B9** — document the clock/`CONTROL` handoff state (folded into the B3 handover contract):
  hand off at reset-state clocks (app reconfigures) and ensure `CONTROL` is in reset state
  (MSP, privileged) before the branch.

---

## Strengths (keep)

- ADR-0007 D3 prime-bias asymmetry ("cold-when-warm loses the handshake — safe;
  warm-when-cold faults — dangerous; therefore bias to prime") — excellent, and a ready-made
  blog paragraph.
- ADR-0008 D1 layered recovery (digest fail-safe → BREG counter → knock window, each covering
  a distinct failure) — real defense-in-depth.
- Honesty notes (CRC32 = integrity not authenticity; system not secure until the lifecycle
  burn) — the maturity that separates this from hobby work.
- Lifecycle-gated knock window — the dev backdoor self-closes in production. Sharp.

---

## Action checklist

- [x] B1 — remove the step-4 counter increment in ADR-0008; single-source it in ADR-0007 D4
- [x] B2 — make D4 explicit: sw-reset + `.noinit` programming pattern ⇒ clear; else increment;
      classify hibernate-wake ahead of / independent from generic reset cause
- [x] B3 — write the FBL handover contract (disabled+pending-cleared, VTOR/MSP/CONTROL set;
      app re-enables/re-inits) into ADR-0008 D4
- [x] B4 — define the digest covered range to exclude integrity fields (M1 digest; M4 hash+sig)
- [x] B6 — reserved an App partition alongside the FBL block in the backup-domain register map (ADR-0007 D6)
- [x] B7 — MISRA deviation row for the asm jump helper (noted in ADR-0008 D4; row lands with the port code)
- [x] B8 — extend vector sanity (MSP in SRAM bounds + 8-byte aligned)
- [x] B9 — document clock/`CONTROL` handoff state (folded into the B3 handover contract)

## Note for the blog (M1 failure chapter)

B1 (a contradiction between two docs written in the same session) and B3 (an assumption about
app startup that's usually false — even though the resolution was to *keep* app-side init and
just document the contract) are both "design review caught what a quick read wouldn't" moments.
B2/B3 also show the review *being wrong* and corrected by hardware knowledge — an honest,
credible arc for the chapter.

## Further reading
See `docs/references.md` (RTOS/Cortex-M) and `docs/architecture/overview.md` §3 for the boot/app
architecture and memory map.
