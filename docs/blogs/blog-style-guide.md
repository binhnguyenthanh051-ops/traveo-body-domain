# Blog Style Guide — Medium posts for this project

Standing rules for drafting the Medium blog posts, captured so they don't have to be
re-stated each milestone. Each rule has a short **name** so it can be referenced
("apply *Milestone Lens*", "did you *Ship a User Manual*?").

Worked examples in this repo:
- **Bring-up-log style:** `docs/briefs/M1-bringup_log.md`, `docs/briefs/M2-bringup_log.md`
- **Design-essay style:** `docs/blogs/m3-reprogramming-blog-draft.md`

---

## 1. Milestone Lens — pick the post's *type* first

Before writing, decide which lens the milestone deserves:

- **Bring-up log** — chronological, per-seam, *"what the silicon did that the
  datasheet didn't."* Best when the milestone's value was getting hardware to behave
  (M1, M2). Format per finding: *what I expected · what the silicon did · the lesson.*
- **Design essay** — architecture-first, structured around the design rather than the
  timeline. Best when the milestone's value is a *design* — a layered stack, a
  protocol, a security boundary (M3 reprogramming). The bring-up findings become
  supporting colour inside the design narrative, not the spine.

State the lens explicitly in the intro ("the M1/M2 write-ups were bring-up logs; this
one is a design essay") so the reader knows what they're getting.

## 2. Design-Essay Skeleton — the section order for a design post

When the lens is *design essay*, use roughly this skeleton (adapt as needed):

1. **The shape of the problem** — why this milestone is hard / interesting; the core
   design tension in one or two sentences.
2. **Architecture** — the structure, *emphasising the two or three subsystems that
   carry the milestone* (for M3: CAN + Diag). Name the layers and the dependency
   rules.
3. **Key decision structures** — the boot/decision trees, the state machines, the
   handshakes. The load-bearing logic.
4. **The main sequence** — the headline flow end-to-end (for M3: the download).
5. **Performance** — see *Numbers Or It Didn't Happen*.
6. **User manual** — see *Ship A User Manual*.
7. **What I'd add / the thesis** — the architectural bet the milestone makes, and the
   forward link (see *Forward Link + Prediction*).

## 3. Numbers Or It Didn't Happen — always a real performance section

A design/systems post must quantify. Include, with **measured** values (mark clearly
anything that is estimated or *illustrative*):

- Headline throughput (e.g. MB/min **and** KB/s).
- The governing sizes: transfer block size, erase size, program/row size, data
  transfer size, buffer sizes.
- The governing timing params (e.g. ISO-TP **STmin**, bit rates, block size).
- **Where the time goes** — split the cost across the real contributors (wire vs.
  flash vs. round-trip, etc.), so optimisation is reasoned, not guessed.
- **What would make it faster**, ranked by payoff, including the honest "this is a
  silicon floor" items.

Put the parameter values in a **table** (see *Tables For Parameters*).

## 4. Ship A User Manual — how to drive, test, and verify

Every post ends with a practical section a reader could follow:

- **How to trigger / start** the thing (all the routes in, if there are several).
- **How to run / test** it — the exact command(s), and representative output.
- **How to verify success** — the concrete observable that proves it worked.
- **The failure / safety demo** — how to deliberately break it and show the safe
  behaviour. This is often the most valuable paragraph; don't skip it.

## 5. Diagram-Per-Concept — visualize heavily, match the type to the content

Lean on diagrams; a professional post is mostly pictures with prose between them.
**Match the diagram type to what it shows:**

| Content | Diagram type |
|---|---|
| Software architecture / components | **component / layered flowchart** |
| A protocol or request/response flow | **sequence diagram** |
| Stateful logic (sessions, locks, modes) | **state machine** (`stateDiagram`) |
| A decision with branches | **decision-tree flowchart** (clearer than a literal state machine for pure decisions — say so if asked why) |
| Memory / image / packet layout | **block diagram** |
| A cost/effort breakdown | **pie or bar chart** |

Conventions:
- **Use Mermaid.** It matches the repo's other docs and renders on GitHub /
  [mermaid.live](https://mermaid.live).
- **Note the Medium caveat once, near the top:** Mermaid doesn't render on Medium —
  export each diagram to PNG/SVG and insert as an image.
- **Color-code consistently across all diagrams** so they reinforce each other
  (e.g. blue = host-testable/pure-logic, amber = target/silicon, green = go/safe,
  red = stop/fail, purple = next-milestone).
- A one-line italic caption under each diagram, saying what to take from it.

## 6. Honest Labels — call out demonstration-grade vs. production

Explicitly flag what is mechanism-not-production: e.g. "CRC32 proves integrity, **not**
authenticity," "this seed/key is not real crypto," "a *simplified* variant of the
standard." Precision about what's demonstration-grade earns more trust than implying
certification the project doesn't have. Tie these to where the real version lands
(usually a later milestone).

## 7. Architect Voice — decisions with reasoning and alternatives

- **Name the patterns** used (Layered architecture, Strategy, State, Command/handler)
  — they're the vocabulary that makes decisions defensible.
- For each significant choice, give the **reasoning and at least one alternative
  considered** — the "why not X" is often more interesting than the "why Y."
- Connect to the bigger arc: what interview/architecture story each decision supports.

## 8. Tables For Parameters — tabulate config and performance values

Any set of related constants (bit rates, sizes, timing params, allocations) goes in a
table, not prose. Easier to scan, and it reads as engineering.

## 9. Forward Link + Prediction — end pointing forward

Close by connecting to the next milestone, and — where the architecture makes a
testable bet — **state the prediction in writing** (e.g. "M4's signature check should
plug into the verify seam without the download flow changing at all; if it doesn't,
the seam was in the wrong place"). It makes the series feel designed, and it's honest
accountability for the architecture.

---

*Add rules here as new preferences come up, same as the MISRA deviation log grows —
a living guide, not a one-time snapshot.*
