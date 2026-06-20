# AI-assisted workflow

How this project divides work across AI tools, and the rules that keep the output
something *I* can stand behind in an interview.

## Division of labor

| Tool | Owns | Does NOT do |
|------|------|-------------|
| **Claude Code** | Architecture, interfaces, ADRs, design review, multi-file/conceptual debugging, design of the hard cores (scheduler, IPC, security), blog-chapter drafting, architecture diagrams | Mass-generate function bodies I haven't reasoned about; edit files another agent is mid-edit on |
| **GitHub Copilot** | Function bodies *after* the signature + contract exist, Unity test cases, boilerplate, repetitive refactors | Invent interfaces/architecture; write the tricky core logic unsupervised |
| **MS Copilot** | General concept questions, "what does this MISRA rule mean", quick syntax lookups | Anything touching repo code or design — it has no project context |

## Handoff rule (Claude Code ↔ Copilot)

Claude Code defines the **contract** — signature, preconditions, MISRA constraints, the ADR
behind it. Copilot fills the **body**. Claude Code **reviews** before commit. If a function
needs design judgment or spans files, it's Claude Code's even though Copilot could attempt it.

## The comprehension gate (most important rule)

The purpose of this project is to prove *I* can architect and explain automotive software.
So: **never commit a line I can't explain in an interview.** Every AI output is a draft I
must understand and be able to defend. "The AI wrote it" is not an acceptable answer to
"walk me through this." Rejected AI suggestions and *why* are good blog material.

## Roles assigned beyond the obvious

- **Blog drafting → Claude** (Code or chat), from ADRs + commit history + session
  transcripts. Not MS Copilot — no context.
- **Design red-teaming → Claude Code.** Explicitly ask it to attack the design: find the
  race, the replay hole in authenticated CAN, the power-loss window in the EEPROM emulator.
- **MISRA/deviation upkeep → Claude Code proposes, I decide.** On a cppcheck flag, it
  offers fix-vs-deviate; I choose and it updates `docs/coding-standard.md`.

## Test framework: Unity (pure C)

Host unit tests use **Unity** (ThrowTheSwitch) — pure C, no C++ dependency, which keeps the
host build aligned with the MISRA-C production code. Test harnesses themselves are out of
MISRA scope (see `docs/coding-standard.md`). The skeleton currently ships a tiny hand-rolled
assert harness in `common/messages/tests/`; migrating it to Unity is an early task.

## Context isolation (keep tools from drifting)

Each tool sees a different slice: Claude Code has `CLAUDE.md` + repo; Copilot sees open
files; MS Copilot sees nothing. Keeping `CLAUDE.md` and the ADRs current is what stops
Claude Code — and the code it shapes for Copilot — from drifting off the architecture.
