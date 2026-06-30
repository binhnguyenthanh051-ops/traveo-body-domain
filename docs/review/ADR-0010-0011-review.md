# Design review — M2 ADRs: ADR-0010 (FreeRTOS integration), ADR-0011 (CANFD config), ADR-0007 extension (D7–D9, App-side `.noinit`)

**Reviewed:** 2026-06-28 · **Status:** discussed — all findings actioned into the ADRs the
same session. A second wave of **silicon corrections** (from on-board bring-up) is recorded
below and folded into the ADRs + `docs/briefs/M2-bringup_log.md`.
**Verdict:** the core decisions held up — heap-forbidden allocation as a *link-time* guarantee
(ADR-0010 D2), the BASEPRI/`__NVIC_PRIO_BITS` interrupt model (D3), the M2-8 "decoded struct
over an interface, never a queue handle" seam (D6), and the prime-biased `.noinit` ownership
(ADR-0007 D9). The review and bring-up findings were completeness/correctness fixes, not
reversals. None of the findings changed an architectural direction.

---

## Important (medium)

### 1 — ADR-0010 memory budget omits the second transport queue
**Severity:** medium (the budget under-counts; an undeclared queue).

The architecture splits CAN handling across **two** tasks — `CAN_CyclicTask` decodes/routes,
`App_CyclicTask` runs the state machine — so a decoded `body_msg_t` must cross between them on
a queue. The budget listed only `raw_frame_q` (ISR → `CAN_CyclicTask`); the second queue was
implied but unnamed.

**Fix:** name `app_msg_q` (`CAN_CyclicTask` → `App_CyclicTask`, decoded structs) in ADR-0010
D5 and add it to the budget. (~0.2 KB; small, but the budget must be exact since the heap is
forbidden.)

### 2 — ADR-0007 D8: the app linker is BSP-generated and can be regenerated away
**Severity:** medium (a silent revert reintroduces the M2-3 hazard).

D8 pinned `.noinit` by editing `app_cm4.ld`, which ModusToolbox **generates** — a BSP
re-generation could silently revert the `INCLUDE noinit.ld` + length edit and put the
handshake back at a floating address, exactly the cross-image mismatch the pin exists to
prevent.

**Fix:** the app **forks `app_cm4.ld` into a repo-owned linker** (as the FBL owns its linker),
points `LINKER_SCRIPT` at the fork, and adds a **link-time `ASSERT`** on the handshake address.
Regeneration cannot touch the fork; the `ASSERT` fails the build if the address ever drifts.

---

## Polish (low / nit)

### 3 — ADR-0010 D5: "cyclic/hybrid" mislabels the health task
`Health_CyclicTask` has **no input queue**, so "block-on-queue-with-period-timeout" doesn't
apply to it — it is plain-periodic (`vTaskDelayUntil`). The hybrid model is right for
`CAN_`/`App_CyclicTask` only. **Fix:** state that health is plain-periodic so the next reader
doesn't go hunting for its queue.

### 4 — ADR-0011: the message-RAM footprint is a constraint, not a footnote
RX FIFO 0 (8 × ~72 B FD elements) + filters + 1 TX buffer must fit the **channel's
message-RAM allotment**, which on M_CAN is small and shared. **Fix:** elevate it to an explicit
sizing line (~700 B; first lever if it doesn't fit is RX FIFO depth 8→4) rather than a "verify
in TRM" aside.

### 5 — ADR-0010: timer-daemon stack/queue not pinned
With `configUSE_TIMERS=1` the daemon-task stack is a real budget item; leaving
`configTIMER_TASK_STACK_DEPTH` / `configTIMER_QUEUE_LENGTH` implicit risks a silent overflow if
a callback grows. **Fix:** pin the values.

### 6 — ADR-0007 D8: the 256 B `.noinit` rationale contradicted D6 (nit)
The draft said 256 B "leaves room for the reserved App partition" — but **D6 puts the App
partition in BREG, not `.noinit`**. **Fix:** reword — the handshake is 16 B; the 256 B is
alignment/headroom, not an App register block.

---

## Silicon corrections (post-implementation — review against the board)

These are ADR claims the bring-up *disproved or refined*. Each is folded into the named ADR
section and journaled in `M2-bringup_log.md`.

### S1 — ADR-0010 D7: "the app may assume `VTOR == app_base`" is **false**
The FBL sets `VTOR = app_base` at the jump, but Cypress `SystemInit` then **relocates `VTOR`
to the RAM vector table** (`.ramVectors`) for runtime ISR registration — so by `main()` it is
*not* the flash base. A `configASSERT(SCB->VTOR == APP_VECTOR_BASE)` trapped on this.
**Corrected** D7 to accept the RAM-vectors address or the flash base.

### S2 — ADR-0011 D3: the CM4 RX interrupt needs the **TRAVEO NVIC mux**
The original wiring (`Cy_SysInt_Init({canfd_irqn, 5})` + `NVIC_EnableIRQ(canfd_irqn)`) is the
PSoC 6 form. On TRAVEO T2G the CM4 reaches peripheral interrupts through an **8-channel NVIC
mux**; `intrSrc` must pack a mux channel (`<< CY_SYSINT_INTRSRC_MUXIRQ_SHIFT`) with the system
interrupt, and `NVIC_EnableIRQ` takes the **mux channel**. The bare-IRQn form silently mapped
to `NvicMux0` + enabled a dead line → "init OK, no RX". **Added** to D3.

### S3 — ADR-0011 D2: RX FIFO 0 with zero elements drops every frame
The configurator generated `numberOfFIFOElements = 0`; routing was correct but the FIFO had no
storage, so RF0N never fired. A reminder that "enable FIFO 0" and "size FIFO 0" are two
settings. (Captured in the bring-up log; D2 already requires a non-zero element count.)

### S4 — ADR-0007 D8: "pin `.noinit`" means pin the handshake's **own** section
`.noinit` is a shared catch-all — the BSP `system_psoc6` (~`0x5A0` B) + `cyhal` + `cy_syslib`
land first, so pinning the whole output section put the 12-B handshake at an **offset**
(`0x0801_FCA0`, inside the reserved 2 KB), matching across images only by coincidence of
identical layouts. **Corrected:** the handshake lives in a dedicated **`.fbl_handshake`**
section pinned at `_noinit_start`; the general `.noinit` floats back in `ram`. (Caught by
reading the `.map`, not by it "working".)

---

## Strengths (keep)

- **Heap forbidden as a compile-time guarantee** (ADR-0010 D2) — `DYNAMIC_ALLOCATION=0` makes a
  stray `xTaskCreate` a *link* error, not a review promise. The strongest line in the milestone.
- **The M2-8 seam** (D6) — the state machine consumes decoded structs through a pure interface;
  the host build (plain GCC, no FreeRTOS headers) is what *structurally* prevents the kernel
  leaking in. The M2-5 host test (N reprograms ⇒ no boot-loop trip) proved on x86 before silicon.
- **Single-source `.noinit` pin** (`noinit.ld` INCLUDEd by both linkers + `ASSERT`s) — a future
  address mismatch is a build failure, not a field bug.
- **Prime-biased ECC ownership** (ADR-0007 D9) — App consumes, FBL primes; no first-read fault,
  no race.

---

## Action checklist

- [x] 1 — name `app_msg_q` + add to the ADR-0010 budget
- [x] 2 — fork `app_cm4.ld` into a repo-owned linker + link-time `ASSERT` (ADR-0007 D8)
- [x] 3 — clarify `Health_CyclicTask` is plain-periodic (ADR-0010 D5)
- [x] 4 — message-RAM footprint as an explicit sizing constraint (ADR-0011 D2)
- [x] 5 — pin `configTIMER_TASK_STACK_DEPTH` / `configTIMER_QUEUE_LENGTH` (ADR-0010)
- [x] 6 — reword the 256 B `.noinit` rationale (ADR-0007 D8)
- [x] S1 — correct the VTOR handover claim (ADR-0010 D7)
- [x] S2 — record the TRAVEO NVIC-mux requirement (ADR-0011 D3)
- [x] S3 — RX FIFO 0 element-count gotcha (bring-up log)
- [x] S4 — dedicated `.fbl_handshake` section (ADR-0007 D8)

## Note for the blog (M2)

The pre-implementation review (1–6) was housekeeping. The **silicon corrections (S1–S4)** are
the chapter: each is a "the datasheet/ADR said X, the chip did Y" moment, and three of them
(VTOR relocation, the NVIC mux, the `.noinit` handshake at the wrong offset) only surfaced
because something cheap — an assert, a stage counter, the `.map` — was in place to catch them.
S2 is the sharpest: I *flagged* the NVIC mux as the watch-for in the ADR, then shipped the
bare-IRQn form anyway, and the `tx`-vs-`isr` stage counters localized it in one flash.

## Further reading
`docs/briefs/M2-bringup_log.md` (the design-vs-silicon journal), `docs/briefs/M2-design-decisions.md`
(the decision index + task table + SRAM map), and the ADRs themselves for the actioned text.
