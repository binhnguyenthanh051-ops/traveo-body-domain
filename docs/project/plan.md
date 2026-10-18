# Project plan — Sep 2026 → Feb 2027

**Re-planned:** 2026-09-23, after a month-long break. **Updated 2026-09-27 (W39 retro):** M5 closes
in W40, not W48; rules 1 and 5 loosened, rule 10 added. Supersedes the dates in `docs/roadmap.md`
(which still holds the topic inventory and the episode list).

**Why re-plan:** the job search starts **Dec 2026 – Feb 2027**, not June 2027. The remaining
roadmap (M5 + M6 + Track S + extras) was sized for June and does not fit. It does not need to:
M1–M4 are built on silicon, tagged and published — already a strong portfolio.

**The principle:** be **ready to apply by Nov 1** (was Oct 25; moved at the W42 retro for M5B). Everything after that makes the portfolio
stronger, but nothing on the bench is allowed to delay the job search.

---

## Rules

1. **Hardware on the weekend by default; weekday bench is negotiated.** The default is still
   boards on Saturday, with weekdays for code, host tests, firmware *builds*, docs and writing.
   A weekday bench evening is allowed when that week's plan says so, agreed at the Sunday retro
   (e.g. W40, closing M5). Every bench session, weekday or not, runs from a prepared checklist.
2. **Plan every week, day by day.** Each Sunday produces next week's daily plan in
   `docs/project/weeks/YYYY-Www.md`. Only the coming week is planned per day; later weeks stay at
   the level of the calendar below (a daily plan ten weeks out would be fiction).
3. **One small delivery or blog every 1–2 weeks.** A delivery is something someone else could
   see: a published post, a tag, a README, a log capture, a demo trace.
4. **Retrospective every Sunday at 10:00.** 30–45 minutes, template below, written into the week file.
5. **Budget ~12 h/week; burst to ~20 h when there is more time.** A public holiday or a lighter
   day-job week can carry a burst of up to ~20 h (likely W41). Bursts are planned at the Sunday
   retro, not improvised mid-week. A plan that needs your best weeks fails on your average ones.
6. **Today's task is done today.** Only sickness or an urgent matter moves it; write the reason
   in the retro. The time box still holds.
7. **No new detours before Dec 6.** A new idea gets one line in `docs/future-ideas.md`. A
   *finding* (something wrong in existing work) gets a row in `docs/project/findings.md`.
8. **Commit at the end of every session**, even work in progress.
9. **ADR/REQ ceremony only for decisions that will appear in a post.** Glue and bring-up shims
   get a commit message and a cheap test.
10. **Finished early? Offer the next task, don't stop me.** If today's task is done before
   **22:30**, ask whether I want to continue with the next day's task. 22:30 is the hard stop.
   A task pulled forward is marked in the week file, and its original day becomes lighter or free.
11. **Definition of Done (DoD).** *(Added 2026-10-18.)* A milestone is **done**, and gets its tag,
   only when all nine checks hold:
   1. Every requirement it owns traces to a **design** decision (ADR sub-decision).
   2. …is **implemented** (`@impl REQ-…` in code).
   3. …is **tested** (`@test REQ-…` in a host test).
   4. …and, for target behaviour, has **silicon evidence** (a bench step + a capture in `docs/bench/`).
   5. The **traceability report** generated from those tags (`docs/traceability-convention.md` §3)
      shows no unexplained gap and is committed with the tag.
   6. No requirement holds only on a host fake or has an Open finding against it. If one does, close
      it or **defer it explicitly** (reason + target milestone in the requirements file, said plainly
      in the post).
   7. The work is merged to `main` through a **PR with CI green**. The tag is set on `main`, with a
      **GitHub Release** from it (short notes + the traceability report).
   8. The **blog post draft is reviewed**: every hardware or result claim traces to the report or a
      capture. Publishing is *not* part of the DoD; it is rule 3's delivery and follows the tag.
   9. **Review evidence** is in `docs/review/M<n>-review.md`, in three sections: requirements
      (complete, testable, unambiguous), design (each ADR decision covers its REQs, alternatives
      recorded) and tests (each `@test` really checks what its REQ says, fail paths included).
      Each section has a checklist, findings with IDs (`M<n>-R1`…), a verdict and a date. Findings
      are closed, or moved to `findings.md`, before the tag. It's a solo project, so the record
      says so honestly: *owner review, with Claude as adversarial reviewer*, not an independent one.

## The weekly rhythm

| Day | Time | What |
|---|---|---|
| Mon–Thu | ~1 h each | Code, host tests, firmware builds, docs, blog writing. No board unless the week's plan says so (rule 1). |
| Fri | — | **Off.** |
| Sat | 3–4 h | **Bench session** (when the week has one), from a checklist prepared Mon–Thu. |
| Before any bench day | — | Both firmware images build clean from the committed tree (F-003). |
| Sun | ~1.5 h | **10:00 — retrospective (45 min)** + plan next week + optional light writing. A scheduled Claude task pre-fills the facts and drafts the plan at 10:00; you add energy/feelings and adjust. |

About 10–12 hours a week; ~20 in a planned burst week (rule 5).

---

## Calendar to Dec 6

| Week | Dates | Weekday focus | Bench | Delivery |
|---|---|---|---|---|
| **W39** | Sep 23–27 | Commit the M5 pile; plan + roadmap in repo | Smoke test: both kits flash, boot and log after 5 weeks away | **Clean repo + plan** ✓ |
| **W40** | Sep 28–Oct 4 | **Close M5** — Stages 0–6 on silicon, findings into the ADRs | Mon 28 + Sat 3 | **M5 closed, tag `m5-secoc`** ✓ |
| **W41** | Oct 5–11 | ~~*(burst, ~14 h)* README v1 · M5a post · M6 design plan · flash-script fix (F-012/F-013)~~ **Cancelled**; its work moved one week back | — | — |
| **W42** | Oct 12–18 | Publish **M5a post** (restart week, ~8 h) | *(none)* | **M5a post** ✓ |
| **W43** | Oct 19–25 | **M5B — SecOC closure** *(burst ~14 h)*: persistent freshness store (REQ-SECOC-006/007/008), F-010, traceability report, M5 review record (rule 11) | **Sat:** replay-after-reset refused, Stage 7 resync, Stages 5–6 regression | **M5B merged to `main`** (report + review record) |
| **W44** | Oct 26–Nov 1 | M5B post draft reviewed → **tag `m5b-secoc` + GitHub Release** (DoD done) · **README v1** "start here" · F-012/F-013 · F-002, F-005, F-007, F-009 | **Sat:** triage F-006/F-008 | **✅ Ready to apply** (README v1 on `main`) |
| **W45** | Nov 2–8 | Draft + publish the **CM7 bring-up post** (the old "M5b post": cache/non-cacheable mailbox) + the M5B story (the reset-then-replay demo now fails) · series index post | — | **CM7 post** |
| **W46** | Nov 9–15 | **M5 demo post** from the W40 + W43 captures (forgery, replay, cross-ID, replay-after-reset) | — | **M5 demo post** |
| **W47–W49** | Nov 16–Dec 6 | Interview prep: "10 decisions I can defend" cheat-sheet from the ADRs, mock design questions. M6-lite starts early (see below). Planned week by week at the retros. | — | — |

**Shifted 2026-10-18 (W42 retro), twice:** W41 was cancelled, which pushed everything back one week.
Then the requirements audit found REQ-SECOC-006/007/008 not satisfied on target, so W43 became
**M5B** (close SecOC under the new rule 11) and README v1 moved to W44. The ready-to-apply gate
slips from Oct 25 to **Nov 1**, and interview prep has three weeks, not five.

**Buffer logic:** M5 closed in W40, so the W41–W42 reserve Saturdays weren't needed. The M6 design
plan planned for W41 was never written (there is no `docs/briefs/M6-brief.md` yet).
The cut-off is Wednesday of W40: if Stage 3 has not passed by then, W40 ends at Stages 0–4, and
Stages 5–6 take the W41 Saturday.

## Dec – Feb: M6-lite, while applying

Mostly writing on purpose, because interviewing takes time and energy too.

- **Threat-model essay** — safety + security as one defensive story (EP.21 + EP.24 merged).
- **Capstone retrospective** (EP.26) — what I'd do differently; the overview as centrepiece.
- *Optional, only with spare energy:* watchdog + safe-state hands-on (EP.22).

Planned week by week at the Sunday retros, like everything else.

## Cut / parked (moved to `docs/future-ideas.md`, nothing deleted)

| Item | Why |
|---|---|
| Track S — S.3/S.4 | Standalone; doesn't serve the Dec window |
| EEPROM emulator | SecOC persistence across reboot stays a documented limitation |
| EP.20 — app-side UDS | The roadmap itself says diagnostics isn't the differentiator |
| EP.23 — fault injection | Covered by M4 Seam 6 (dead CM0+) + the M5c forged/replay demo |
| EP.25 — cache essay | Becomes a section of the M5b post — hands-on now, on the CM7 |
| BVT bench, Pi bridge, checks pipeline | Regression-testing infrastructure; regression testing isn't the blocker |

---

## Sunday retrospective template

Copy into the bottom of the week file.

```markdown
## Retrospective (Sun YYYY-MM-DD)

**Hours:** planned __ / actual __      **Energy (1–5):** __

**Done:**
-

**Not done — and why:**
-

**Delivery this week:** shipped / slipped (→ where it moved)

**What helped:**
-

**What got in the way:**
-

**Findings:** opened __ / resolved __ · any Open past its date? (re-date or reject)

**One change for next week:**
-

**Next week's delivery:**
```
