# Automotive Embedded Architect Portfolio — Roadmap

**Board:** Infineon CYTVII-B-E-1M-SK (CYT2B7 — dual-core 160 MHz Cortex-M4 + 100 MHz Cortex-M0+, 1 MB code flash, 96 KB work flash, 128 KB SRAM)
**Goal:** Build a Medium series + GitHub portfolio demonstrating *architecture-level* thinking — not protocol checklists — to support a move into an automotive embedded software architect role in Vietnam.
**Format inspiration:** Toby (@johnehk86) — episodic, narrative, failures included, Claude Code shown as a visible collaborator.
**Cadence:** ~20 hrs/week (incl. weekends), 12-month horizon.

---

## 1. Guiding principles

1. **Depth over breadth.** Deep, hands-on topics beat shallow ones. CAN/Diag are *not* a focus — you already know them; mention briefly, don't anchor chapters on them.
2. **One flagship system, growing over time.** Every chapter adds to the same running codebase (a 2-node body-domain network), not disconnected demos. This is what makes the series read as "architecture," not "tutorials."
3. **Be honest about what the hardware proves.** This board is asymmetric dual-core (M4 + M0+), no cache. Topics outside what it can prove (true symmetric multicore, cache behavior) are written as *design/comparative essays*, clearly labeled — not faked as hands-on. Revisit hands-on once/if a Body High (Cortex-M7) board is added.
4. **Claude Code is a visible collaborator, not a ghostwriter.** Show prompts, show what it got wrong, show how you corrected it — especially for architecture/design decisions, not just code generation.
5. **Every chapter ends with a decision, not just a result.** "It worked" is a tutorial. "Here's the tradeoff I chose and why" is architecture.
6. **Architecture for testability.** Logic-heavy modules (scheduler core, EEPROM emulation, message packing) are written hardware-independent and unit-testable on the host, with chip-specific code behind interfaces. This is what makes host-side CI possible — and is itself an architect-level structural decision worth writing about.

---

## 2. Topic inventory (scoped to this board)

| Topic | Status | How it's used |
|---|---|---|
| Custom preemptive scheduler (your own, bare-metal) | **Core, hands-on** | Spine of Arc 2 — your strongest differentiator |
| FreeRTOS (after your own scheduler exists) | **Core, hands-on** | Comparative chapter: "I built mine, then ported FreeRTOS — what I'd actually choose and why" |
| EEPROM emulator on work-flash | **Core, hands-on** | Arc 3 — wear-leveling, sector management, power-loss safety |
| Fail-safe / fault handling | **Core, hands-on** | Arc 4 — watchdog, safe-state design, fault injection |
| Asymmetric dual-core (M4 + M0+) IPC | **Core, hands-on** | Threaded through Arc 2–4 — task partitioning, shared-memory protection, hardware semaphores |
| **Security — secure boot + flash protection** | **Core, hands-on** | Arc 2 — the M0+ secure boot chain, flash/access protection on this chip |
| **Security — crypto / HSM on the M0+ core** | **Core, hands-on** | Arc 2 — offloading crypto to the security core, ties into the dual-core IPC story |
| **Security — authenticated CAN messages (SecOC-style)** | **Core, hands-on** | Arc 3 — signed/authenticated bus comms between Node A and Node B |
| **CI — host-side build + test on every push** | **Core, hands-on** | Introduced in Arc 1, grows every arc as testable modules are added |
| CAN bus (node-to-node) | Supporting, brief | 1 chapter — the "wire" the system runs on, not the subject |
| UDS diagnostics | Supporting, brief | 1 chapter — diagnostic session design as *architecture* (why OEMs separate diag from app logic), not protocol mechanics |
| Design patterns (HAL abstraction, state machines, pub/sub) | **Woven through every chapter** | No standalone chapter — called out inline wherever applied |
| Cache handling | **Dropped for now** | Revisit with a Cortex-M7 (Body High) board later |
| True symmetric multicore | **Dropped for now** | Revisit with a multi-M7 board later |
| Functional safety (ASIL concepts) | Design essay only | Framing/comparative chapter in Arc 4; explicitly not claiming ISO 26262 compliance work |

---

## 3. Flagship project

**A 2-node automotive body-domain network**, mirroring a real gateway + actuator-ECU architecture:

- **Node A — "Gateway"**: owns CAN arbitration, runs UDS diagnostics, hosts your custom scheduler, aggregates signals, runs the secure-boot chain and crypto offload to the M0+.
- **Node B — "Actuator node"**: simulated window/door/light controller, publishes sensor data, consumes (authenticated) commands over CAN.
- **(Stretch) Node C**: PC-based cluster simulator via USB-CAN, or a second physical board if budget allows later.

The system grows in sophistication across all four arcs — each arc adds a layer rather than starting a new demo. **Security and CI are not separate projects — they thread into this same system.**

---

## 4. Chapter roadmap

Denser than the 10 hr/week version: ~26 chapters across the same 4 arcs / 12 months. Security and CI are interleaved, not bolted on at the end.

### Arc 1 — Foundations, bring-up + CI from day one (Months 1–2)
*Goal: working hardware, working build+test pipeline, the system is "alive" and every push is green.*

- **EP.01** — Board bring-up: first blink, first debug session, project skeleton with Claude Code (`CLAUDE.md` setup story)
- **EP.02** — Fundamentals: automotive E/E architecture 101 — why gateways and domain controllers exist
- **EP.03** — **CI from day one**: host-side build on every push, and *why* an embedded project should be testable on x86 from the start (the architecture-for-testability argument)
- **EP.04** — Design patterns primer: the HAL abstraction layer you'll reuse — and how it's what lets logic run host-side under CI
- **EP.05** — First CAN frame: Node A sends, you receive on a PC tool. Brief — plumbing, not the point.

### Arc 2 — Scheduler, dual-core + security foundations (Months 3–6)
*Goal: the technical core of the portfolio + the secure-boot/crypto story.*

- **EP.06** — Why build your own scheduler? What FreeRTOS hides, and why an architect should know it anyway
- **EP.07** — Building a minimal preemptive scheduler on the Cortex-M4 from scratch (context switching, SysTick, priorities)
- **EP.08** — The failure chapter: whatever breaks first (stack corruption, priority inversion, a race) — lean into it, highest-engagement content
- **EP.09** — Unit-testing the scheduler core on the host under CI (proving the testability argument pays off)
- **EP.10** — Scheduler at work: Node A's gateway tasks (CAN RX, diagnostics, housekeeping) running under your scheduler
- **EP.11** — Asymmetric dual-core: handing work to the Cortex-M0+, IPC via hardware semaphores, shared-memory protection design
- **EP.12** — **Security: secure boot + flash protection** — the M0+ secure boot chain on this chip, access protection, what "root of trust" actually means here
- **EP.13** — **Security: crypto / HSM on the M0+** — offloading crypto to the security core, designed as a clean service behind an interface (ties into the dual-core IPC work)
- **EP.14** — FreeRTOS comparison: port the same workload, compare design tradeoffs honestly — "what I'd choose for a real project and why"

### Arc 3 — Memory, diagnostics + secure comms (Months 7–9)
*Goal: production-flavored subsystems, second node online, the bus gets trustworthy.*

- **EP.15** — Fundamentals: how real automotive EEPROM emulation works (wear-leveling, sector rotation, power-loss safety)
- **EP.16** — Building your own EEPROM emulator on work-flash, from scratch — host-testable logic under CI
- **EP.17** — Node B comes online: actuator node joins the bus, publishes/consumes signals
- **EP.18** — **Security: authenticated CAN (SecOC-style)** — signing/verifying messages between Node A and Node B, freshness/replay protection, using the M0+ crypto service from EP.13
- **EP.19** — UDS diagnostics architecture: why diagnostics is a separate layer from application logic; session design on the gateway (brief — design-focused)
- **EP.20** — CI grows up: PC-side integration tests (host drives the protocol logic, asserts responses) running on every push
- **EP.21** — Architecture essay: "how I'd scale this to a real 10-ECU vehicle network" — your most architect-flavored post

### Arc 4 — Fail-safe, threat model + retrospective (Months 10–12)
*Goal: resilience + security story tied together, capstone.*

- **EP.22** — Fundamentals: functional safety concepts (ASIL framing, fault-tolerance patterns) — framed as design literacy, not compliance work
- **EP.23** — Watchdog + safe-state design on the gateway node
- **EP.24** — Fault injection: kill Node B, observe and harden the gateway's response
- **EP.25** — Threat-model essay: secure boot + authenticated comms + safe-state combined into one defensive story — "how safety and security design reinforce each other"
- **EP.26** — Cache & true multicore: comparative design essay (no hands-on claim) — what changes on M7-class silicon, why an architect should know it even without the hardware
- **EP.27** — Capstone: full architecture document for the 2-node system + retrospective — "what I'd do differently" (the reflective judgment architect interviews probe for)

---

## 5. Working notes

- Keep a `CLAUDE.md` per repo with: board/chip facts, architecture decisions log, conventions. Treat Claude Code session transcripts as raw material for chapters.
- One GitHub repo for the flagship system (not one repo per chapter) — README shows the current architecture diagram, updated as it grows.
- The repo is structured so hardware-independent logic lives in host-testable modules (scheduler core, eeprom_emu, message packing) with chip code behind interfaces — this is what the CI pipeline builds and tests.
- Don't write a chapter until the thing in it actually works (or actually failed instructively). This roadmap is a backbone, not a fixed schedule — let real build problems reorder things.

---

## 6. Open items / revisit later

- Body High (Cortex-M7) board purchase — unlocks cache + symmetric multicore hands-on content (year 2 candidate)
- Hardware-in-the-loop CI (auto-flash + on-target tests) — natural extension of the host-side CI once it's mature
- Third node / PC cluster simulator — stretch goal if Arc 3 runs ahead
- AUTOSAR-flavored layer — only if year-1 content lands well and time allows
