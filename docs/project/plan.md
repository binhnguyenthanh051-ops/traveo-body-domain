# Project plan — Sep 2026 → Feb 2027

**Re-planned:** 2026-09-23, after a month-long break. Supersedes the dates in `docs/roadmap.md`
(which still holds the topic inventory and the episode list).

**Why re-plan:** the job search starts **Dec 2026 – Feb 2027**, not June 2027. The remaining
roadmap (M5 + M6 + Track S + extras) was sized for June and does not fit. It does not need to:
M1–M4 are built on silicon, tagged and published — already a strong portfolio.

**The principle:** be **ready to apply by Oct 25**. Everything after that makes the portfolio
stronger, but nothing on the bench is allowed to delay the job search.

---

## Rules

1. **No hardware on weekdays.** Boards are flashed and probed on the weekend only. Weekday work
   is code, host tests, firmware *builds* (building needs no board), docs and blog writing — so
   every Saturday bench session is pure execution from a prepared checklist.
2. **Plan every week, day by day.** Each Sunday produces next week's daily plan in
   `docs/project/weeks/YYYY-Www.md`. Only the coming week is planned per day; later weeks stay at
   the level of the calendar below (a daily plan ten weeks out would be fiction).
3. **One small delivery or blog every 1–2 weeks.** A delivery is something someone else could
   see: a published post, a tag, a README, a log capture, a demo trace.
4. **Retrospective every Sunday at 10:00.** 30–45 minutes, template below, written into the week file.
5. **Budget ~12 h/week; 15–20 is upside.** A plan that needs your best weeks fails on your
   average ones.
6. **Every weekday has a no-energy option.** If an evening is bad, do the smallest item or skip —
   and write down that you skipped. Skipping is data, not failure.
7. **No new detours before Dec 6.** A new idea gets one line in `docs/future-ideas.md`.
8. **Commit at the end of every session**, even work in progress.
9. **ADR/REQ ceremony only for decisions that will appear in a post.** Glue and bring-up shims
   get a commit message and a cheap test.

## The weekly rhythm

| Day | Time | What |
|---|---|---|
| Mon–Thu | ~1 h each | Code, host tests, firmware builds, docs, blog writing. **No board.** |
| Fri | — | **Off.** |
| Sat | 3–4 h | **Bench session** (when the week has one), from a checklist prepared Mon–Thu. |
| Sun | ~1.5 h | **10:00 — retrospective (45 min)** + plan next week + optional light writing. A scheduled Claude task pre-fills the facts and drafts the plan at 10:00; you add energy/feelings and adjust. |

About 10–12 hours a week.

---

## Calendar to Dec 6

| Week | Dates | Weekday focus (no HW) | Saturday bench | Delivery |
|---|---|---|---|---|
| **W39** | Sep 23–27 | Commit the M5 pile; plan + roadmap in repo | Smoke test: both kits flash, boot and log after 5 weeks away | **Clean repo + plan** |
| **W40** | Sep 28–Oct 4 | README → 2-minute "start here" | *(rest — optional)* | **README v1** |
| **W41** | Oct 5–11 | Draft the **M5a post** (SecOC design, proven by host tests) | Stage 2: AES-CMAC known-answer test on each CM0+ | — |
| **W42** | Oct 12–18 | Finish + publish M5a post | Stage 3.1–3.2: Node B dual-core boot + loopback unaffected | **M5a post** |
| **W43** | Oct 19–25 | Write the RX bring-up shim; series index post; README final | Stage 3.3–3.4 + shim: every receive verdict on the UART | **✅ Ready to apply** |
| **W44** | Oct 26–Nov 1 | Write up the single-node capture | Stage 4: cross-node key agreement | **M5b log capture** |
| **W45** | Nov 2–8 | Phase B prep: transceiver EN pin (schematic), `CAN_LOOPBACK_TEST=0` builds, VN1610 config, termination plan | Stage 5: real bus, happy path end to end | — |
| **W46** | Nov 9–15 | **M5b post** (CM7 bring-up + cache/non-cacheable mailbox — absorbs the EP.25 cache essay) | Stage 6: forgery + replay demos with the VN1610 | **M5b post** |
| **W47** | Nov 16–22 | Prepare demo-capture checklist | Stage 7: resync; capture the demo traces | — |
| **W48** | Nov 23–29 | Draft the **M5 demo post** | *(buffer — reserve bench)* | **Tag `m5`** |
| **W49** | Nov 30–Dec 6 | Publish demo post; "10 decisions I can defend" cheat-sheet from the ADRs | *(buffer)* | **M5 demo post** |

**Buffer logic:** W48–W49 Saturdays are reserve bench days. If a stage slips, it moves there —
not into a weekday.

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

**One change for next week:**
-

**Next week's delivery:**
```
