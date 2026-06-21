# Traveo Body Domain — a 2-node automotive body network

A portfolio project exploring **automotive embedded software architecture** on the
Infineon TRAVEO™ T2G CYT2B7 (board: `CYTVII-B-E-1M-SK`). It is the running system
behind a Medium series: each chapter adds a layer rather than starting a new demo.

> This is a learning/portfolio project, not production automotive software. Where it
> imitates production patterns (secure boot, SecOC-style authentication, EEPROM
> emulation), that is called out as *design intent*, not a compliance claim.

## The system

```
        CAN bus
  ┌───────────────────────┐
  │                       │
┌─┴──────────────┐   ┌────┴───────────────┐
│ Node A         │   │ Node B             │
│ "Gateway"      │   │ "Actuator node"    │
│                │   │                    │
│ - bootloader   │   │ - door/light/window│
│ - app:FreeRTOS │   │   state machine    │
│ - secure boot  │   │ - publishes sensors│
│ - crypto (M0+) │   │ - consumes (auth'd)│
│ - signal aggr. │   │   commands         │
└────────────────┘   └────────────────────┘
```

Both nodes run on the same chip family; Node A is the richer "gateway" build.

## Why the layout looks like this (the architecture-for-testability bet)

Hardware-*independent* logic lives in `shared/` modules that compile and unit-test on a
normal x86 host under CI:

| Module | What it is | Host-testable? |
|--------|------------|----------------|
| `shared/messages` | CAN signal pack/unpack, message specs | ✅ pure logic |
| `shared/can`      | CAN driver (config-varied per image) | interface host-testable; driver on-target |
| `shared/diag`     | UDS stack (programming vs data/DTC services per image) | logic host-testable |
| `shared/eeprom_emu`| work-flash EEPROM emulation | ✅ logic over a flash *interface* |
| `shared/crypto`   | thin wrapper over vetted primitives (HW crypto / M0+) | wrapper logic testable; keys on-target |
| `shared/secoc`    | SecOC-style message authentication | ✅ framing logic |
| `shared/hal`      | hardware abstraction interfaces | ✅ interfaces; impls on-target |
| `scheduler`       | custom preemptive scheduler (standalone deep-dive, ADR-0005) | ✅ core logic; context switch on-target |

Chip-specific code sits behind `shared/hal` interfaces, so the logic above never `#include`s
a vendor header directly. That separation is what makes host CI possible — and is itself a
deliberate architectural decision (see `docs/architecture/decisions`).

## Build & test (host)

```bash
make test     # builds and runs all host-side unit tests
make clean
```

The firmware itself is built with Infineon ModusToolbox (see node READMEs); only the
hardware-independent modules build on the host.

## Repo map

- `shared/` — driver/logic modules compiled into images with per-variant config:
  `hal`, `messages`, `can`, `diag` (UDS), `eeprom_emu`, `crypto`, `secoc`
- `scheduler/` — custom preemptive scheduler (standalone deep-dive, not in product images)
- `node_a_gateway/` — gateway ECU: `bootloader/` (FBL + secure boot) and `app/` (FreeRTOS)
- `node_b_actuator/` — actuator ECU: `app/`
- `host_tools/` — Python: CAN tooling, UDS client, integration test scripts
- `docs/` — `architecture/overview.md`, ADRs, roadmap, coding standard, workflow, briefs,
  reviews, `references.md`
- `.github/workflows/ci.yml` — host build + test + static analysis on every push

## Chapter ↔ module map

See `docs/roadmap.md` for the full series outline. Each module's own README notes which
episode introduces it.

## Contributing / Git workflow

This repo follows a lightweight Git workflow — see **`GitRules.md`** for branch naming,
Conventional Commits, milestone tagging, PR hygiene, and the secrets rule.

`main` is protected: changes land via pull request, and the CI status checks
(`host-build-test`, `static-analysis`) must pass before merge — so `main` stays green.
