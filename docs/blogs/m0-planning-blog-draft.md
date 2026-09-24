<!--
DRAFT — M0 / "Planning" post: how I work, the project overview, and what I gain.
- Visual-first by design: short prose between figures. Redraw the [FIGURE] diagrams in
  Excalidraw (hand-drawn style reads as authentic thinking-out-loud — good for this post).
- Do a voice pass so it sounds like you. Keep it tight; let the figures carry the load.
- Figures already designed in chat: reuse them.
-->

# How I'm building an automotive platform — with four AI tools and my own judgment

*The opening post of a series. Before any bootloader or bus, here's how the work actually
gets done — the workflow, the repo, and, honestly, what I'm getting out of it.*

I'm building a small automotive body-domain platform from scratch — two ECUs, a bootloader,
secure boot, diagnostics, an RTOS — on real Infineon TRAVEO™ T2G hardware. Every design
decision is written down; every milestone ships with tests, a bring-up log, and a post like
this one. This first post isn't about the silicon. It's about *how I work* — because that,
more than any single feature, is what I want the series to demonstrate.

## The workflow: four AI tools, one human at the gate

I use four AI tools, and the whole thing only works because each has a lane — and because the
judgment never leaves mine.

[FIGURE 1 — AI tool division of labor: Human (judgment) over Claude Chat (design), Claude Code
(build), Copilot (bodies), MS Copilot (lookups). Already drawn in chat.]

- **Claude Chat** is my design partner and reviewer — architecture discussions, tradeoffs,
  and the reviews that catch bugs *before* the board arrives.
- **Claude Code** builds inside the repo — scaffolding, implementation, refactors — against a
  design I've already agreed to.
- **GitHub Copilot** fills function bodies and test cases once the contract exists.
- **Microsoft Copilot** answers the throwaway questions that never touch the repo.

The rule that holds it together: **I never commit a line I can't explain in an interview.**
The AI drafts and challenges; I decide. That's not a slogan — it's the reason this portfolio
is defensible instead of ghostwritten.

## The loop I run every milestone

The tools plug into a loop that repeats for each milestone — design, decide, test, build,
check, and only then ship. The key move is that a human-judgment gate sits between the
automated checks and shipping.

[FIGURE 2 — Per-milestone design loop (brief → design → ADR → failing tests → implement →
checks → human review → bring-up → blog), with the review feeding findings back. Already
drawn in chat.]

Two things fall out of this loop that I lean on hard:

- **Design-first, always.** The decision and its alternatives get written as an ADR
  (Architecture Decision Record) *before* code. Those reviews have repeatedly caught
  "correct on paper, wrong in behaviour" bugs while they were still cheap.
- **Machines check what machines wrote.** Lint, MISRA, unit tests, and a small traceability
  pipeline gate every merge — deterministic checks that can't do my thinking, only catch
  what I forgot.

[FIGURE 3 — Layered check pipeline: deterministic gates (block merge) vs advisory LLM reviewer
(never blocks), both feeding human judgment. Already drawn in chat.]

## The project at a glance

[FIGURE 4 — Roadmap: Done (M0–M4) → Now (M5–M6) → Future (Pi bridge, QNX, M7 port). Already
drawn in chat.]

The system is a two-node body-domain network: a **gateway** and an **actuator**, on TRAVEO™
T2G. So far it has a flash bootloader, secure boot, UDS reprogramming, and an application on
FreeRTOS — each its own milestone, each with a post. What's next is the second node with
authenticated messaging, then a resilience pass.

## What comes after M6

The platform doesn't end at M6. The threads I've deliberately parked for later:

- **A Linux ↔ MCU bridge** on a Raspberry Pi — inter-unit communication over a low-speed
  link, extending the story toward Linux-based compute.
- **QNX** — a microkernel IPC demo and an honest comparison against FreeRTOS and Linux.
- **Porting the gateway to M7-class silicon** — and measuring what my hardware-abstraction
  bet actually cost when the chip changed. That last one is the real test of the architecture.

## What I actually gain from this — the point

I'm building this because of what it *makes me*, not just what it produces. Concretely:

[FIGURE 5 — What I gain (five capability boxes, each with its practice). Already drawn in chat —
make this one big; it's the payoff.]

- **Platform & security architecture** — from building a bootloader, secure boot, and UDS by hand.
- **RTOS internals mastery** — from writing a preemptive scheduler from scratch *before* using FreeRTOS.
- **Architecture & documentation judgment** — from ADRs and design reviews on every decision.
- **Engineering rigor** — from host-testable design, CI, MISRA, and a check pipeline that gates merges.
- **AI-augmented development** — from orchestrating four AI tools while keeping every judgment my own.

And beyond M6, **Linux/QNX and multi-core breadth** from the bridge and the M7 port.

That's the real deliverable. The bootloader and the bus are how I get there — but what I walk
away with is the ability to *architect* a platform, *document* the reasoning, and *direct* AI
without outsourcing the thinking. The rest of the series is the evidence.

---

*Next: the bootloader — and the bug that only crashed when I unplugged the debugger.*
