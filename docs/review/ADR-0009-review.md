# Design review — ADR-0009 (scheduler design)

**Reviewed:** 2026-06-20 · **Status:** open — findings to be actioned before/with EP.07 implementation.
**Verdict:** strong design. Clean core/port split, static allocation and BASEPRI are the
right MISRA/ASIL-aligned choices, and the per-decision FreeRTOS comparisons are
architect-grade framing. Three correctness issues below would bite on hardware (two are
"correct on paper, silently wrong in behaviour"); the rest are footguns and polish.

---

## Must-fix

### F1 — D5: initial EXC_RETURN mislabeled; "extended frame" claim contradicts the conditional FP save
**Severity:** high (context-switch bug, not host-testable — only surfaces on-target).

The ADR sets `EXC_RETURN = 0xFFFFFFFD` but calls it "extended frame" and states "all tasks
default to the extended frame." `0xFFFFFFFD` has **bit 4 = 1 → standard (non-FP) frame**
(Thread mode, PSP). The extended/FP value is `0xFFFFFFED` (bit 4 = 0).

The claim also contradicts the design's own optimization: D5 steps 3/6 save `{s16–s31}`
*only when* bit 4 = 0, to avoid FP overhead for non-FP tasks. If every task starts extended,
that condition is always true for fresh tasks and the optimization is dead.

**Fix:** keep `0xFFFFFFFD` (it's correct), relabel it **standard/non-FP**, and **delete**
the "all tasks default to extended / 18 extra words" sentences. Tasks transition to the
extended frame automatically on first FPU use (hardware sets `CONTROL.FPCA`; next exception
entry builds the extended frame; the per-task saved EXC_RETURN captures bit 4 = 0 and the
conditional save engages). This is the FreeRTOS behaviour. The value is right; the prose
describes a different, worse design.

### F2 — D3: `__builtin_ctz(0)` is UB; no idle task guarantees a non-empty ready set
**Severity:** high (garbage task selection the moment all tasks block).

When no task is ready, `ready_bitmap == 0` and `__builtin_ctz(0)` is undefined behaviour
(garbage index on ARM). The design has no idle task, so the first time every task is blocked
on `sched_delay()`, selection reads garbage.

**Fix:** add a built-in **idle task** at the lowest priority, always READY, running a `WFI`
loop. It keeps the ready set non-empty by construction (UB gone) and is the natural home for
low-power `WFI`. FreeRTOS creates exactly this for the same reason.

### F3 — D7: round-robin among equals never actually preempts
**Severity:** medium (a documented feature is silently a no-op).

D1 rotates the running task to the back of its priority list each tick, but D7 step 4 pends
PendSV only "if a higher-priority task just became ready." Rotating the list without pending
PendSV changes nothing visible — the rotated-in task won't run until some other switch
occurs. Equal-priority time-slicing therefore does nothing.

**Fix:** in `sched_tick()`, also pend PendSV when rotation at the **current** priority level
changes the front task (i.e. when >1 task is ready at the running level).

---

## Footguns (call out in code + ADR)

### F4 — BASEPRI must be written pre-shifted; ISR priorities must respect the ceiling
**Severity:** medium (the most common Cortex-M critical-section bug).

Cortex-M implements only the top `__NVIC_PRIO_BITS` priority bits, so
`SCHED_MAX_SYSCALL_PRIORITY` must be shifted (`prio << (8 - __NVIC_PRIO_BITS)`) before it
goes into BASEPRI — writing the raw number silently mis-masks. Corollary: every interrupt
that calls into the scheduler (SysTick, later CAN, …) must be numerically ≥ the ceiling
(lower urgency); PendSV stays lowest. State this platform constraint explicitly in the ADR.

### F5 — 8-byte stack alignment for the initial frame
**Severity:** low (rare, alignment-dependent faults that look random).

AAPCS/STKALIGN require an 8-byte-aligned stack at exception entry.
`sched_port_init_stack()` must align the stack top to 8 bytes before writing the fake frame.

### F6 — PC slot bit 0 in the initial frame
**Severity:** low.

Mask bit 0 of the entry address when writing the stacked PC slot (Thumb state comes from
xPSR.T, already set via `0x01000000`), so a stray bit 0 doesn't cause issues.

---

## MISRA / polish

- **F7 — language extensions need deviations.** `__builtin_ctz` and the port inline asm are
  compiler/language extensions (Dir 1.1 / Rule 1.2; Rule 4.3 wants asm encapsulated). The
  coding standard currently exempts only test harnesses — add a deviation row for each in
  `docs/coding-standard.md` rather than leaving them silent.
- **F8 — critical section: save/restore previous BASEPRI.** Exit currently writes
  `BASEPRI = 0`. Save/restore-previous is barely more code and removes a whole class of
  future nesting bugs; do it now.
- **F9 — `sched_delay(0)` is undefined.** Decide: yield, or no-op. Document it.

---

## Strengths (keep)

- Mechanism-for-mechanism alignment with FreeRTOS (bitmap, BASEPRI, PendSV/lazy-stacking)
  makes the S.4/EP.14 comparison about framing, not mechanism — smart and honest.
- D7's delta-list vs O(N)-scan table: "I know the production optimization and chose not to
  build it, here's why" — exactly the reasoning interviews probe for.
- FPU lazy-stacking is addressed at all (F1 is the only detail off), which puts this ahead
  of most from-scratch schedulers.

---

## Action checklist

- [ ] F1 — relabel EXC_RETURN as standard; delete extended-default claim (D5)
- [ ] F2 — add idle task (lowest priority, always READY, WFI) (D3/D2)
- [ ] F3 — pend PendSV on equal-priority rotation (D7)
- [ ] F4 — shift BASEPRI ceiling; document ISR-priority constraint (D6)
- [ ] F5 — 8-byte align initial stack (D2/D5 port)
- [ ] F6 — mask PC bit 0 in initial frame (D5)
- [ ] F7 — add MISRA deviation rows for `__builtin_ctz` + port asm
- [ ] F8 — critical section save/restore previous BASEPRI (D6)
- [ ] F9 — define `sched_delay(0)` behaviour (D7)

## Note for the blog (S.3)

F1 and F3 are both "correct on paper, silently wrong in behaviour." That the *design review*
caught them before the board arrived — rather than a debugger at 2am — is itself the
argument for design-first, and is worth narrating that way in the failure chapter.
