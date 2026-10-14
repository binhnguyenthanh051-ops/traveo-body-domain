# Blog Style Guide — Medium posts for this project

Standing rules for the Medium posts, so they never have to be re-stated. Each rule has a
short **name** to reference ("apply *Short And Visual*", "is it *Ready To Upload*?").

**The reference post is M5** — follow it before anything else:
- Published: <https://medium.com/@binh.nguyenthanh051/m5-signing-every-can-frame-between-two-mcus-a9d6fa9ca94f>
- Source: `docs/blogs/m5-secoc-blog-draft.md` · images: `docs/blogs/img/m5/`
- Previous post in the same format: M4, <https://medium.com/@binh.nguyenthanh051/m4-secure-boot-on-a-dual-core-mcu-8fff4f2015b3>

Older worked examples (pre-v2 style, longer): bring-up logs `docs/briefs/M1-bringup_log.md`,
`M2-bringup_log.md`; design essay `docs/blogs/m3-reprogramming-blog-draft.md`.

*v2, 2026-10-14 — rewritten after the M5 post, from the author's review of the draft.*

---

## 1. Post Template — the fixed frame of every post

Same frame every time, in this order:

1. **Title:** `Mx — <what it does, in plain words>` (e.g. "M5 — Signing every CAN frame between two MCUs").
2. **Subtitle (italic, one line):** *Part &lt;Roman numeral&gt; of Traveo II Domain — building an
   automotive body-domain network from scratch on an Infineon TRAVEO™ T2G, with the design done
   in the open and Claude used as a visible collaborator.*
3. **Hero image** (see *Hero Image*).
4. **Intro, 3 short paragraphs max:** pick up the **previous post's promise** in its own words
   ("M4 ends with a promise: …"), one line contrasting this milestone with the last one
   ("M4 checks which code runs. M5 checks which commands that code obeys."), and the plan plus
   the twist ("the test has a twist: …").
5. **Numbered sections** (see *Problem-Solution-Implementation-Test*), subheadings phrased as
   questions where natural ("What can an attacker do to a CAN frame?").
6. **User manual** (see *Ship A User Manual*).
7. **Metadata** block, as a code block:
   ```
   Board      · …
   Toolchain  · ModusToolbox, Python
   RTOS       · …
   Crypto/Bus · …
   Repo       · tag mX-…
   Written    · <Month Year>
   ```
8. **Follow along**, exactly:
   ```
   💻 GitHub — Traveo II Body Domain
   🔗 https://github.com/binhnguyenthanh051-ops/traveo-body-domain (tag mX-…)
   💻 LinkedIn
   🔗 https://www.linkedin.com/in/binh-nguyen-thanh-2b807714b/
   ```

Medium tags (5): Embedded Systems, Automotive, Cybersecurity (or the topic's own),
CAN Bus / the relevant bus, Microcontrollers.

## 2. Problem-Solution-Implementation-Test — the section order

1. **The problem** — what goes wrong without this milestone, shown as a picture first.
2. **The solution** — the idea/protocol/mechanism, one fix per problem, before any code.
3. **The implementation** — how it plugs into the platform built so far; what is new vs reused.
4. **The test** — host first, then silicon. The bench surprise ("all host tests passed, and
   the first real bus rejected every frame") is the story's peak; put it here, set it up in
   the intro.
5. **User manual** — then Metadata and Follow along.

The forward link to the next milestone is **one line** inside an Honest note or the user
manual's failure demo ("That's exactly what M6 fixes."), not a closing essay.

## 3. Short And Visual — pictures first, few words

- **A diagram per idea.** M5: 9 diagrams + hero in ~1,000 words of prose. Aim for that ratio.
- **Paragraphs of 1–3 sentences.** If a paragraph needs more, it needs a diagram or a list.
- **Every figure gets a one-line italic caption** saying what to notice.
- **No tables** — Medium can't render them. Parameters go in a small **spec block**
  (a code block with aligned `Name   value` lines, like M4's "The cost"); comparisons go in a
  diagram or a 3-bullet list.
- **Cut, don't add:** no "alternatives considered" sections, no design-pattern names, no
  limitations section. One alternative is fine **inline** when it explains a choice in one line.
- Target **6–8 min read**.

## 4. Plain Human Voice — write like the author talks

- Simple grammar, everyday words, short sentences. First person ("I expected 15 bytes. The
  frame had 16.").
- **Avoid AI phrasing:** "load-bearing", "let's dive in", "it's worth noting", "crucially",
  "seamless", "robust", "leverage", "delve", "in essence", "not just X but Y", rhetorical
  question chains, and stacks of em-dashes.
- One idea per sentence. Bold only the word that carries the point.
- Keep the author's small personal lines ("The replay is my favourite: …").

## 5. Honest Labels — one line each, two or three per post

Format, exactly as in M4/M5: a plain paragraph starting **`Honest —`**, one sentence, e.g.
*"Honest — both nodes share one key, built into the M0+ image. Real cars use a key per ECU."*
Put each next to the thing it qualifies. **No roll-up / limitations section.**

## 6. No Internal IDs — readers don't know our numbering

No requirement IDs (`REQ-…`), ADR numbers, finding numbers (`F-0xx`, runbook `#17`), Jira-style
references, or repo file paths in the story text. Name the thing instead ("the freshness
rule", "the attacker script"). Code identifiers only where the reader would type or see them
(commands in the user manual, a real log line).

## 7. Numbers Or It Didn't Happen — measured, or say so

- Every number comes from a **capture, log or test count** in the repo (`docs/bench/<date>/`,
  the runbook). Use the count **as it was at the moment described** (M5: 230 tests at the
  first bus run, 239 after the fix).
- Not measured → write **"not measured yet"**. Never present an estimate as a measurement
  (M4's "suppose it takes 5 ms" should have been labelled *estimated*).
- A demo not yet run on the bench is worded as expected behaviour ("It **should** be
  accepted"), never as a result.
- Name the right mechanism (M5: the **FreeRTOS stack-overflow hook** caught it; the watchdog
  only reset the chip after).

## 8. Ship A User Manual — how to drive, test, and verify

A short practical section a reader could follow:
- **How to run** it — the exact command, one line.
- **What success looks like** — the concrete observable, ideally **5 real log lines** from the
  bench capture, each tagged with what it shows (`← replay`).
- **The failure demo** — how to break it on purpose and what happens. Often the best paragraph.

## 9. Diagram Conventions — Mermaid source, PNG on Medium

- **Write diagrams in Mermaid** in the draft. Match the type to the content: layered
  flowchart for architecture, sequence diagram for request/response, decision-tree flowchart
  for decisions, a row of boxes for a frame/memory layout, before/after pair for a bug.
- **Same colours in every post:** blue `#dbeafe/#3b82f6` = host-testable logic ·
  amber `#fff7ed/#f59e0b` = board/silicon · green `#dcfce7/#22c55e` = accept/safe ·
  red `#fee2e2/#ef4444` = attack/reject · purple `#ede9fe/#8b5cf6` = next milestone ·
  grey `#f3f4f6/#9ca3af` = padding/unused.
- Keep labels short (2–4 words per line, `<br/>` for a second line); long labels make
  Mermaid stretch diagrams sideways. Stack before/after cases vertically
  (`flowchart TB`, `direction LR` inside each subgraph, `A ~~~ B` to order them).
- **Check every rendered PNG** before publishing (order, wrapping, colours).

## 10. Hero Image — same look as M4/M5

Dark teal grid background (`#0f1a1f`), amber mono caption **`TRAVEO T2G · MILESTONE Mx`**,
a big white serif headline (2 short lines), an amber underline, a one-line grey italic
subtitle, and a small line-art diagram on the right in the post's colours. Template:
`docs/blogs/img/m5/hero.svg` (1400×734) — copy it, change the words and the diagram.
**No bench photos** as hero.

## 11. Ready To Upload — the publishing pipeline

A post is ready only when it has **zero `TODO`s** (the author wants an upload-ready file, not
a list of holes). Then:

1. `python host_tools/blog/render_diagrams.py pages docs/blogs/<post>-blog-draft.md docs/blogs/img/<mx>`
2. Open `docs/blogs/img/<mx>/_measure.html` in the browser pane; read `window.__sizes` into
   `docs/blogs/img/<mx>/sizes.json`.
3. `python host_tools/blog/render_diagrams.py shoot docs/blogs/img/<mx>` — renders `fig-NN.png`
   (2×) and `hero.png` from `hero.svg` with headless Edge. Look at each PNG.
4. `python host_tools/blog/md_to_medium.py docs/blogs/<post>-blog-draft.md --img-base https://raw.githubusercontent.com/binhnguyenthanh051-ops/traveo-body-domain/<branch>/docs/blogs`
   — tables → lists, Mermaid → the PNGs, editor notes dropped.
5. Commit on a `docs/<mx>-blog` branch and **push** (raw URLs only work once pushed); check all
   image URLs return 200.
6. The author opens `docs/blogs/<post>-medium.html`, **Ctrl+A → Ctrl+C → paste** into a new
   Medium story (Medium re-hosts the images), adds the tags, publishes.

Don't rely on mermaid.ink for the paste: it drops images when several are requested at once.

## 12. Review The Last Post — fix it before writing the next

Before drafting a post, skim the previous published one for typos, a wrong board or fact,
and estimates shown as measurements, and list fixes for the author (M4 had "Coretex",
"hasing", "Authencicity", and "Body High Kit" for a Body Entry board).

## 13. Milestone Lens — still pick the type, quietly

Design-essay milestones follow *Problem-Solution-Implementation-Test*. A pure bring-up
milestone can tell the story as findings — *what I expected · what the silicon did · the
lesson* — inside the same frame (*Post Template*). Don't announce the lens in the intro.

---

*A living guide: when the author gives feedback on a draft, add or change the rule here in the
same session, so it never has to be said twice.*
