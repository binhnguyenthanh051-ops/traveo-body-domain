# Project plan — Sep 2026 → Feb 2027

**Re-planned:** 2026-09-23, after a month-long break. **Updated 2026-09-27 (W39 retro):** M5 closes
in W40, not W48; rules 1 and 5 loosened, rule 10 added. Supersedes the dates in `docs/roadmap.md`
(which still holds the topic inventory and the episode list).

**Why re-plan:** the job search starts **Dec 2026 – Feb 2027**, not June 2027. The remaining
roadmap (M5 + M6 + Track S + extras) was sized for June and does not fit. It does not need to:
M1–M4 are built on silicon, tagged and published — already a strong portfolio.

**The principle:** be **ready to apply by Oct 25**. Everything after that makes the portfolio
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
| **W40** | Sep 28–Oct 4 | **Close M5:** Stage 0 builds, Stage 5 prep (EN pin, VN1610, termination) · ~20 h | **Tue/Wed + Sat:** Stages 1–6 (secure-boot regression → CMAC KAT → cross-core MAC → key agreement → real bus → forgery/replay/cross-ID demos), Stage 7 optional | **M5 closed, tag `m5`** |
| **W41** | Oct 5–11 | *(burst, ~20 h if the week allows)* README v1 "start here" · draft the **M5a post** (SecOC design) · findings | *(reserve — anything that slipped from W40)* | **README v1** |
| **W42** | Oct 12–18 | Publish **M5a post** · draft the **M5b post** (CM7 bring-up + cache/non-cacheable mailbox) | *(reserve)* | **M5a post** |
| **W43** | Oct 19–25 | Publish M5b post · series index post · README final | — | **✅ Ready to apply** |
| **W44** | Oct 26–Nov 1 | **M5 demo post** from the W40 captures (forgery, replay, cross-ID) | — | **M5 demo post** |
| **W45–W49** | Nov 2–Dec 6 | Interview prep: "10 decisions I can defend" cheat-sheet from the ADRs, mock design questions. M6-lite starts early (see below). Planned week by week at the retros. | — | — |

**Buffer logic:** W41–W42 Saturdays are reserve bench days. If a W40 stage slips, it moves there.
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
