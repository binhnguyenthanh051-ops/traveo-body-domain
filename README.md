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
│ - custom sched │   │ - door/light/window│
│ - UDS diag     │   │   state machine    │
│ - secure boot  │   │ - publishes sensors│
│ - crypto (M0+) │   │ - consumes (auth'd)│
│ - signal aggr. │   │   commands         │
└────────────────┘   └────────────────────┘
```

Both nodes run on the same chip family; Node A is the richer "gateway" build.

## Why the layout looks like this (the architecture-for-testability bet)

Hardware-*independent* logic lives in standalone modules that compile and unit-test
on a normal x86 host under CI:

| Module | What it is | Host-testable? |
|--------|------------|----------------|
| `common/messages` | CAN signal pack/unpack, message specs | ✅ pure logic |
| `scheduler`       | custom preemptive scheduler core | ✅ core logic; context-switch asm is target-only |
| `eeprom_emu`      | work-flash EEPROM emulation (wear-levelling, sectors) | ✅ logic over a flash *interface* |
| `security`        | crypto service interface + SecOC-style framing | ✅ logic; key storage is target-only |
| `common/hal`      | hardware abstraction interfaces | ✅ interfaces; impls are target-only |
| `node_a_gateway` / `node_b_actuator` | the actual firmware (ModusToolbox) | ❌ on-target |

Chip-specific code sits behind the `common/hal` interfaces, so the logic above never
`#include`s a vendor header directly. That separation is what makes host CI possible —
and is itself a deliberate architectural decision (see `docs/architecture/decisions`).

## Build & test (host)

```bash
make test     # builds and runs all host-side unit tests
make clean
```

The firmware itself is built with Infineon ModusToolbox (see node READMEs); only the
hardware-independent modules build on the host.

## Repo map

- `common/` — shared HAL interfaces and message definitions
- `scheduler/` — the custom preemptive scheduler
- `eeprom_emu/` — EEPROM emulation over work-flash
- `security/` — secure-boot helpers, crypto service interface, message authentication
- `node_a_gateway/`, `node_b_actuator/` — the two firmware applications
- `host_tools/` — Python: CAN tooling, UDS client, integration test scripts
- `docs/` — architecture notes and decision records
- `.github/workflows/ci.yml` — host build + test on every push

## Chapter ↔ module map

See `docs/roadmap.md` for the full series outline. Each module's own README notes which
episode introduces it.
