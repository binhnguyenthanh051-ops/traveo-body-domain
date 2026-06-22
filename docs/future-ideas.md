# Future ideas (parked)

Deliberately *not* in scope for the current Traveo body-domain project. Parked here so they
can be picked up later without re-deriving the reasoning — and so they don't dilute the
current work. Revisit after M1–M6 (and Track S) are done.

## Sitara (Linux + LPDDR + SNAND, possibly edge AI)
**Status:** parked — candidate *next* project after the Traveo portfolio is complete.

**The idea:** a Sitara AM-class SoC project to learn LPDDR (DDR training / memory controller),
external SNAND boot, embedded Linux (Yocto, device tree, drivers), and possibly a small
on-device AI model.

**Why it's parked, not dropped:**
- It starts a *different* portfolio, not an extension of the Traveo one — it pulls toward the
  **Linux / applications-processor** automotive job family, which hires differently from the
  **MCU / AUTOSAR** track this project targets.
- Running both on ~20 hrs/week risks two shallow stories instead of one deep one — the exact
  scope-dilution we've guarded against. A finished solid project beats an unfinished flashy one.
- "Chip count" / "has Linux" is a weaker signal to an automotive architect interviewer than
  depth and judgment on domain problems (secure boot, scheduler, fail-safe) — which the
  current project already demonstrates.

**If/when picked up — scope it honestly as three distinct sub-topics, don't conflate:**
1. **LPDDR + SNAND** — real, deep board/memory bring-up (DDR training, ext-flash boot). Strong
   and architect-relevant, but a standalone effort.
2. **Linux on Sitara** — large domain (Yocto, device trees, boot chain, drivers). Aligns with
   Linux/AP roles specifically.
3. **Edge AI** — only compelling with a concrete task and ideally an NPU-class part; "ran a tiny
   model on CPU" is a crowded, weak demo on its own.

**Decision gate before starting:** confirm the *target job family* first (MCU/AUTOSAR vs
Linux/AP). If staying MCU/AUTOSAR, Sitara is a breadth side-project, not the main signal. If
moving toward Linux/AP, it becomes the primary portfolio and gets scoped like this project did
(roadmap, ADRs, CI, the works).

## Other parked ideas
- Body High (Cortex-M7) board — unlocks cache + symmetric multicore hands-on (already noted in
  the roadmap's open items).
- Hardware-in-the-loop CI (auto-flash + on-target tests).
- AUTOSAR-flavored layer on the app — only if year-1 content lands well.
