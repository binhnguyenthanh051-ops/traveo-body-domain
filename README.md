# Traveo Body Domain — a 2-node automotive body network

Two Infineon TRAVEO™ T2G boards talk over CAN FD the way two ECUs in a car body domain would:
a **gateway** (Node A) with a verified bootloader, and an **actuator** (Node B) that only moves the
door or the light for a command it can authenticate. The bootloader, the RTOS application, UDS
reprogramming, secure boot, the crypto offload to the Cortex-M0+ and SecOC-style authenticated
CAN are each built, tested on the host, and run on silicon. It is the running system behind a
[Medium series](https://medium.com/@binh.nguyenthanh051): each post adds one layer to the same
system rather than starting a new demo.

> A portfolio project, not production automotive software. Where it borrows production patterns
> (secure boot, SecOC, MISRA C:2012), that is *design intent*, not a compliance claim.

## The system

```
              CAN FD (500 kbit/s nominal, 2 Mbit/s data)
  ┌──────────────────────────────┬────────────────────────────────┐
  │                              │                                │
┌─┴────────────────────────────┐ │ ┌──────────────────────────────┴─┐
│ Node A — gateway             │ │ │ Node B — actuator              │
│ CYTVII-B-E-1M-SK · CYT2B7    │ │ │ KIT_T2G-B-H_LITE · CYT4BF      │
│                              │ │ │                                │
│ CM4 160 MHz                  │ │ │ CM7 (L1 I+D cache)             │
│  ├ FBL: verify app → jump    │ │ │  ├ FreeRTOS app                │
│  └ FreeRTOS app, UDS server  │ │ │  └ door/light state machine    │
│ CM0+ 100 MHz                 │ │ │ CM0+ 100 MHz                   │
│  └ crypto service: ECDSA     │ │ │  └ crypto service: AES-CMAC    │
│    (secure boot), AES-CMAC   │ │ │                                │
└──────────────────────────────┘ │ └────────────────────────────────┘
   0x120 door, 0x121 light  A → B   (authenticated commands)
   0x200 telemetry          B → A   (authenticated)
   0x2F0 freshness sync     B → A   (resync after a Node B reboot)
```

The two nodes are **different parts** on purpose. Node A's M4 has no cache. Node B's M7 does, so
the core-to-core mailbox on Node B has to live in an MPU non-cacheable region
([ADR-0018](docs/architecture/decisions/ADR-0018-ipc-transport.md) D6). Full picture:
[`docs/architecture/overview.md`](docs/architecture/overview.md).

## What is proven on silicon

| Milestone | What runs on the boards | Evidence |
|---|---|---|
| **M1** bootloader | FBL checks the app image, then sets VTOR/MSP and jumps; a `.noinit` handshake survives reset | tag [`m1-bootloader`](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/tree/m1-bootloader) · boot capture [`node_a_w39_reset.txt`](docs/bench/2026-09-26/node_a_w39_reset.txt) (`LOG_EVT_APP_JUMP 0x10040000`) |
| **M2** application | FreeRTOS app on Node A, alive and logging | tag [`m2-app-mvp`](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/tree/m2-app-mvp) · [`node_a_w39.txt`](docs/bench/2026-09-26/node_a_w39.txt) |
| **M3** reprogramming | UDS over ISO-TP on CAN FD: a new app image is downloaded into the FBL | tag [`m3-reprogramming`](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/tree/m3-reprogramming) · [ADR-0012](docs/architecture/decisions/ADR-0012-uds-diagnostic-stack.md), [ADR-0013](docs/architecture/decisions/ADR-0013-isotp-transport.md) |
| **M4** secure boot | the FBL refuses an app whose ECDSA signature fails; verification runs on the CM0+ | tag [`m4-secure-boot`](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/tree/m4-secure-boot) · [post](https://medium.com/@binh.nguyenthanh051/m4-secure-boot-on-a-dual-core-mcu-8fff4f2015b3) · [ADR-0016](docs/architecture/decisions/ADR-0016-app-signature-verification.md), [ADR-0017](docs/architecture/decisions/ADR-0017-crypto-service-m0plus.md) |
| **M5** SecOC | every command and telemetry frame carries a CMAC + freshness; forged, bit-flipped, replayed and cross-ID frames are each refused, and the door never unlocks | tag [`m5-secoc`](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/tree/m5-secoc) · [post](https://medium.com/@binh.nguyenthanh051/m5-signing-every-can-frame-between-two-mcus-a9d6fa9ca94f) · [`docs/bench/2026-10-03/`](docs/bench/2026-10-03/) |
| **M5B** persistent freshness | epochs survive power cycles; an old frame replayed after a Node B reset is refused; resync within 20 ms | on `main`, not tagged yet · [`g2_g3_reset_replay.md`](docs/bench/2026-10-24/g2_g3_reset_replay.md), [`epochs_power_cycle.md`](docs/bench/2026-10-24/epochs_power_cycle.md) · [traceability report](docs/traceability.md) |

**Honest gaps.** The capture folders in `docs/bench/` start in September 2026, so for M1–M4 the
evidence is the tagged code and the post; the bench runs then were not archived. M5B is not
tagged: the review ([`docs/review/M5-review.md`](docs/review/M5-review.md)) found a telemetry
deadlock after a Node A reboot (F-014) and a missed save-failure path (F-015). Both are open in
[`docs/project/findings.md`](docs/project/findings.md), and the report shows 3 SecOC requirements
without silicon evidence yet.

## Where to read first

1. [`docs/architecture/overview.md`](docs/architecture/overview.md): the system on one page, the
   memory map, the boot flow.
2. [ADR-0021](docs/architecture/decisions/ADR-0021-secoc-protocol.md) (SecOC protocol) and
   [ADR-0024](docs/architecture/decisions/ADR-0024-freshness-store-two-slot.md) (power-loss-safe
   freshness store): the two ADRs with the most tradeoffs in them. All 23 ADRs are in
   [`docs/architecture/decisions/`](docs/architecture/decisions/).
3. [`docs/traceability.md`](docs/traceability.md): requirement → ADR → code → test → silicon,
   generated by [`host_tools/trace_report.py`](host_tools/trace_report.py). It lists its own gaps.
4. [`docs/review/M5-review.md`](docs/review/M5-review.md): what a milestone review found,
   including the process mistakes.
5. The posts: [medium.com/@binh.nguyenthanh051](https://medium.com/@binh.nguyenthanh051).

## How it is built

Hardware-independent logic lives in `shared/` and compiles with plain GCC on a PC. It reaches the
hardware only through interfaces in `shared/hal`, never through a vendor header
([ADR-0001](docs/architecture/decisions/ADR-0001-host-testable-core.md)). That is what makes the
SecOC freshness rules, the UDS stack and the power-loss cases of the flash store unit-testable
in CI, with fakes for the flash and the crypto core. Each image takes the shared modules with its
own config data rather than `#ifdef`s
([ADR-0004](docs/architecture/decisions/ADR-0004-bootloader-app-split.md)).

| Module | What it is |
|---|---|
| `shared/messages` | CAN IDs, signal pack/unpack, PDU lengths |
| `shared/can`, `shared/diag`, `shared/prot` | CAN FD driver interface, UDS server, ISO-TP |
| `shared/boot`, `shared/sysmgr` | boot decision + `.noinit` handshake, resource lifecycle |
| `shared/crypto` | client side of the CM0+ crypto service (vetted primitives, no hand-rolled crypto) |
| `shared/secoc`, `shared/secoc_store` | SecOC framing + freshness, the two-slot work-flash store |
| `shared/log` | binary target logging, decoded on the host by `host_tools/logview` |
| `shared/hal` | the hardware interfaces, with host fakes for tests |
| `scheduler/` | a preemptive scheduler written as a standalone deep-dive, not linked into any image ([ADR-0005](docs/architecture/decisions/ADR-0005-freertos-app-custom-scheduler-standalone.md)) |

Production C follows MISRA C:2012 ([`docs/coding-standard.md`](docs/coding-standard.md)). Test
harnesses are exempt.

## Build and run the host tests

Needs GCC, GNU make and Python 3.11+; `make lint` also needs cppcheck.

```bash
make test
```

```bash
make lint
```

```bash
python host_tools/trace_report.py --check
```

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs the same `make test` and
`make lint`, plus the Python tool tests, on every pull request and every push to `main`. `main`
only takes merges with CI green.

On Windows with MSYS make, pass a Windows temp folder or GCC can't write its temp files:
`make TMP=C:\path\to\tmp TEMP=C:\path\to\tmp test`.

**Firmware** is built with Infineon ModusToolbox, one project per image:
`node_a_gateway/bootloader` (FBL, CM4 + CM0+), `node_a_gateway/app`, and `node_b_actuator`
(CM7 + CM0+). Flashing is manual. The Node A app must be signed
([`host_tools/sign_image.py`](host_tools/sign_image.py)) or the FBL refuses to boot it.

## Repo map

- `shared/`, `scheduler/`: host-testable modules (above)
- `node_a_gateway/`: `bootloader/` (FBL + secure boot) and `app/` (FreeRTOS)
- `node_b_actuator/`: `proj_cm7/` (application) and `proj_cm0p/` (crypto service)
- `security/`: the SecOC key ID and shared secret, compiled into the CM0+ images only. The secret
  is a **published dummy**: a real fleet provisions per-ECU keys and never commits one
  ([ADR-0021](docs/architecture/decisions/ADR-0021-secoc-protocol.md) D6)
- `host_tools/`: Python: `can_sniff`, `logview`, `secoc_attack`, `uds_flash`, signing, the
  traceability report
- `docs/`: architecture, ADRs, requirements, bench captures, reviews, posts, the project plan

## Git workflow

Branches, Conventional Commits, milestone tags and the secrets rule are in
[`docs/rules/GitRules.md`](docs/rules/GitRules.md). Changes reach `main` by pull request only.
