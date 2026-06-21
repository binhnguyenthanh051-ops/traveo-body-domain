# Automotive Embedded Architect Portfolio — Roadmap

**Board:** Infineon CYTVII-B-E-1M-SK (CYT2B7 — dual-core 160 MHz Cortex-M4 + 100 MHz Cortex-M0+, 1 MB code flash, 96 KB work flash, 128 KB SRAM)
**Goal:** Build a Medium series + GitHub portfolio demonstrating *architecture-level* thinking — not protocol checklists — to support a move into an automotive embedded software architect role in Vietnam.
**Format inspiration:** Toby (@johnehk86) — episodic, narrative, failures included, Claude Code shown as a visible collaborator.
**Cadence:** ~20 hrs/week (incl. weekends), 12-month horizon.
**Architecture:** see `docs/architecture/overview.md`. This roadmap's milestones are the M0–M6 staging from overview §8.

---

## 1. Guiding principles

1. **Depth over breadth.** Deep, hands-on topics beat shallow ones. CAN/Diag are *not* a focus — you already know them; mention briefly, don't anchor chapters on them.
2. **One flagship system, growing over time.** Every chapter adds to the same running codebase (a 2-node body-domain network with a bootloader + app on the gateway), not disconnected demos. This is what makes the series read as "architecture," not "tutorials."
3. **Be honest about what the hardware proves.** This board is asymmetric dual-core (M4 + M0+), no cache. Topics outside what it can prove (true symmetric multicore, cache behavior) are written as *design/comparative essays*, clearly labeled — not faked as hands-on. Revisit hands-on once/if a Body High (Cortex-M7) board is added.
4. **Claude Code is a visible collaborator, not a ghostwriter.** Show prompts, show what it got wrong, show how you corrected it — especially for architecture/design decisions, not just code generation.
5. **Every chapter ends with a decision, not just a result.** "It worked" is a tutorial. "Here's the tradeoff I chose and why" is architecture.
6. **Architecture for testability.** Hardware-independent logic is written host-testable with chip code behind interfaces — what makes host CI possible, and itself an architect-level decision worth writing about.
7. **The design doc is a deliverable.** The architecture exists on paper before all the code does; the overview + ADRs are portfolio artifacts in their own right.

---

## 2. Topic inventory

| Topic | Status | Where it lives |
|---|---|---|
| **Bootloader (FBL) + app split, memory map, VTOR/MSP jump** | **Core, hands-on** | M1 — the system-partitioning spine |
| **Secure boot (ROM root of trust + FBL-verifies-app)** | **Core, hands-on** | M1 (config) → M4 (app verification) |
| **No-init shared RAM handshake (+ ECC priming)** | **Core, hands-on** | M1 — boot/app state across resets |
| **FreeRTOS application** | **Core, hands-on** | M2 — the product RTOS |
| Custom preemptive scheduler | **Core, hands-on** | **Parallel track S** (standalone deep-dive, ADR-0005) |
| FreeRTOS vs custom scheduler comparison | **Core, hands-on** | Track S — concrete A/B |
| **UDS reprogramming (FBL programming services)** | **Core, hands-on** | M3 |
| EEPROM emulator on work-flash | **Core, hands-on** | M2/M3 — wear-levelling, power-loss safety |
| **Security — crypto / HSM offload to M0+** | **Core, hands-on** | M4 — vetted primitives, clean service interface |
| **Security — authenticated CAN (SecOC-style)** | **Core, hands-on** | M5 — A↔B message auth |
| Fail-safe / fault handling, fault injection | **Core, hands-on** | M6 — watchdog, safe-state |
| Asymmetric dual-core (M4 + M0+) IPC | **Core, hands-on** | M4 — crypto offload + shared-memory protection |
| CAN bus (node-to-node) | Supporting, brief | M2 — the "wire," config-varied per image |
| UDS diagnostics (data/DTC, in app) | Supporting, brief | M5/M6 — diag-as-architecture, not protocol |
| Design patterns (HAL abstraction, state machines, pub/sub, config-over-#ifdef) | **Woven through** | Every milestone |
| CI — host build + test + static analysis | **Core, hands-on** | M0, grows every milestone |
| MISRA C:2012 | **Core, hands-on** | M0, enforced throughout |
| Functional safety (ASIL concepts) | Design essay only | M6 — design literacy, not compliance |
| Cache / true symmetric multicore | **Dropped for now** | Comparative essay (M6); revisit with M7 board |

---

## 3. Milestone roadmap

The series follows the M0–M6 staging from `docs/architecture/overview.md` §8. Each
milestone is publishable. Episode numbers are guidance, not a contract — let real build
problems reorder them. The **scheduler runs as a parallel track (S)** that can advance in
gaps (it needs no board for the host-testable core).

### M0 — Foundations (done / in place)
*Repo, host CI, MISRA, and the whole-system architecture on paper.*

- **EP.01** — Why this project, and the architecture up front: the 2-node system, the FBL+app split, the design-doc-as-deliverable stance. (Publish the overview.)
- **EP.02** — Host CI + MISRA from day one: testability as an architectural decision, not an afterthought.
- **EP.03** — Fundamentals: automotive E/E architecture — gateways, domain controllers, why a bootloader exists at all.

### M1 — Bootloader MVP (FBL boots → jumps to app)
*The system-partitioning centrepiece. Needs the board for real flashing.*

- **EP.04** — The memory map: partitioning code flash (FBL vs app), the linker scripts, where the vector tables live. *(verify addresses in TRM)*
- **EP.05** — Bring-up: FBL blinks, first debug session, ModusToolbox project for CYT2B7. (Board has arrived.)
- **EP.06** — The no-init shared RAM handshake — and the ECC-on-uninitialised-SRAM gotcha (cold-boot priming vs warm-reset preservation). Likely a *failure* chapter.
- **EP.07** — The jump: VTOR relocation, MSP set, branch to the app reset vector. What breaks when you get it subtly wrong.
- **EP.08** — ROM secure boot configured: anchoring the root of trust. Why app-level verification is meaningless without it. *(verify mechanism in TRM; document before touching fuses)*

### M2 — Application on FreeRTOS
*A minimal but real app under the product kernel.*

- **EP.09** — Bringing up FreeRTOS on the gateway: first tasks, why FreeRTOS for the product (and what it hides — forward-reference to Track S).
- **EP.10** — CAN online (app config: interrupt-driven), the HAL seam in practice, shared module compiled with an app vs FBL config.
- **EP.11** — EEPROM emulator on work-flash: wear-levelling, sector rotation, power-loss safety — host-tested logic over a flash interface.

### M3 — Reprogramming (UDS in the FBL)
*Field-update path: PC tool flashes the app through the bootloader.*

- **EP.12** — UDS programming-session architecture: why diagnostics/programming is a separate layer; session + security-access design in the FBL.
- **EP.13** — The download sequence (request/transfer/exit + routine), a PC-side Python flash tool, and integration tests in CI.
- **EP.14** — Failure/retrospective: a reprogramming edge case (interrupted download, power loss mid-flash) and how the design recovers.

### M4 — App secure boot + crypto offload
*The bootloader trusts the app; crypto lives on the M0+.*

- **EP.15** — Crypto/HSM on the Cortex-M0+: offloading primitives, a clean `shared/crypto` service interface, the asymmetric dual-core IPC + shared-memory protection story.
- **EP.16** — App image verification in the FBL: manifest format, hash + signature, key handling (secret vs public, storage, lifecycle).
- **EP.17** — Architecture essay: "how I'd scale secure boot + reprogramming to a real multi-ECU vehicle."

### M5 — Second node + authenticated bus
*Node B online; the bus becomes trustworthy.*

- **EP.18** — Node B (actuator) app: door/light/window state machine, publishes/consumes signals.
- **EP.19** — SecOC-style authenticated CAN: MAC + freshness/replay protection between A and B, using the M0+ crypto service.
- **EP.20** — App-side UDS (data/DTC services) as architecture — separation from application logic.

### M6 — Resilience + capstone
*Tie safety and security together; reflect.*

- **EP.21** — Fundamentals: functional-safety concepts (ASIL framing, fault-tolerance patterns) — design literacy, not compliance.
- **EP.22** — Watchdog + safe-state design on the gateway.
- **EP.23** — Fault injection: kill Node B / corrupt a message, observe and harden.
- **EP.24** — Threat-model essay: secure boot + authenticated comms + safe-state as one defensive story — how safety and security reinforce each other.
- **EP.25** — Cache & true symmetric multicore: comparative design essay (no hands-on claim) — what changes on M7-class silicon and why an architect should know it.
- **EP.26** — Capstone: the full architecture retrospective — "what I'd do differently" — plus the updated overview doc as the centrepiece.

### Track S — Custom scheduler (parallel, standalone)
*Runs alongside the milestones; host-testable core needs no board (ADR-0005).*

- **S.1** — Why build your own scheduler? What FreeRTOS hides, and why an architect should know it anyway.
- **S.2** — A minimal preemptive scheduler on the Cortex-M4 from scratch: TCB, ready-set, context switch (PendSV/SysTick, PSP/MSP, the M4F FPU gotcha).
- **S.3** — The failure chapter: the first thing that breaks (stack corruption / priority inversion / a race). Highest-engagement content.
- **S.4** — FreeRTOS comparison: run a representative task set on both, compare design tradeoffs honestly.

(See `docs/briefs/S1-S2-scheduler-brief.md` — the brief that kicks off S.1–S.2.)

---

## 4. Working notes

- Keep `CLAUDE.md` and the ADRs current — they're what keep Claude Code (and the code it shapes for Copilot) on the architecture.
- Treat ADRs, commit history, and Claude Code transcripts as raw material for chapters.
- One GitHub repo; README shows the current architecture, updated as it grows.
- Don't write a chapter until the thing in it works (or failed instructively). Milestones are a backbone, not a fixed schedule.

---

## 5. Open items / revisit later

- Body High (Cortex-M7) board — unlocks cache + symmetric multicore hands-on (year 2).
- Hardware-in-the-loop CI (auto-flash + on-target tests) — natural extension once host CI is mature.
- Third node / PC cluster simulator — stretch if a milestone runs ahead.
- AUTOSAR-flavored layer — only if year-1 content lands well and time allows.
