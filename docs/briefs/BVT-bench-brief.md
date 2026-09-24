# Brief — hardware BVT bench (dev-laptop first, Pi optional later)

> **Goal:** stop hand-reflashing and hand-re-testing M1–M5 every time a feature lands. The
> bench owns both kits, flashes them from the PR's build, and runs a short Build Verification
> Test over CAN. Result posts back as a PR check.
> **Decision (§1.4):** the bench runs on the **dev laptop with the existing VN1610** — no
> purchase, no Pi. A Raspberry Pi is a **Stage-4 option**, bought only if unattended operation
> turns out to matter.
> **Role for Claude Code:** design the harness with the same layering discipline as the
> firmware — bench I/O behind interfaces, protocol logic host-testable with a fake bench,
> tests that name no hardware. Deterministic scripts, no LLM in the gate (same rule as
> `checks-pipeline-brief.md`).

## Scope note — the Pi non-goal stands, for now

`pi-traveo-bridge-brief.md` lists "no CI/CD server on the Pi" as a deliberate non-goal. Since
the bench now lives on the laptop (§1.4), **that non-goal is not violated** and the Pi stays
free for the SPI-bridge project. If Stage 4 ever happens, the two Pi projects will compete for
the same board and wiring — coexistable (different buses), but not while either is mid-bring-up.
Revisit the conflict then, not now.

## What BVT is, and is not

**BVT = "did the build break anything fundamental?"** — ≤10 minutes, runs on every firmware
PR, gates merge.

| Tier | What | Where | When | Budget |
|---|---|---|---|---|
| **T0 — host** | `make test`, `make lint`, `pytest host_tools` *(exists)* | GitHub-hosted | every PR | ~1 min |
| **T1 — BVT** | flash + smoke the real bus, this brief | laptop bench | every firmware PR | ≤10 min |
| **T2 — regression** | full milestone matrix, all attack cases, timing, soak | laptop bench | nightly / pre-tag | ~1 h |

BVT does **not** replace the Unity suites (they catch logic; BVT catches integration), and it
does **not** replace the `M*-bringup-plan.md` runbooks (those are first-light bring-up under
human eyes; BVT is the automated *regression* residue left behind once a stage is green).

**The rule that keeps BVT small:** a test earns its place in T1 only if it fails when
something *fundamental* broke. Anything narrower goes to T2 or to a host unit test.

---

## 1. The two hard constraints (settle these before buying anything)

### 1.1 The Pi cannot build the firmware, and probably cannot flash it either

ModusToolbox ships for Windows x64, Linux **x86_64**, and macOS. There is no aarch64 build,
and Infineon's OpenOCD fork (the one that knows TRAVEO T2G targets) ships inside it. So on a
Pi 5, both `make build` and `make program` are off the table unless a spike says otherwise.

Three ways out, in order of preference:

- **(A) Flash Node A over CAN — already built.** `host_tools/uds_flash/uds_flash.py` drives the
  FBL's full UDS download with nothing but `python-can`. Pure Python, runs anywhere. **The
  deploy step becomes a test of M3+M4**: if the image downloads, verifies its ECDSA signature,
  and the FBL jumps to it, that's the whole secure-reprogramming chain proven before any
  functional test runs. This is the single best asset the repo already has for this.
- **(B) Debugger flashing from the Pi — needs a spike.** `pyocd` runs on aarch64 and speaks
  CMSIS-DAP (KitProg3's protocol); whether it can program CYT2B7/CYT4BF via an Infineon DFP
  CMSIS pack is **unverified — do not assume it**. Needed for anything the FBL can't flash:
  the FBL itself, and all of Node B (no FBL in M5).
- **(C) x86 flashes, Pi tests.** A self-hosted runner on the dev laptop (MTB already
  installed) builds and, if (B) fails, also programs over USB; the Pi does bus + power only.
  Least elegant, zero unknowns.

**(A) and (C) need no spike and cover Stages 1–3.** (B) only matters at Stage 4, when the Pi
enters — so the spike is deferred to there, and the design works either way.

### 1.2 The VN1610 does not move to the Pi

**python-can's `vector` backend is Windows-only.** It `ctypes`-loads `vxlapi.dll` /
`vxlapi64.dll`; there is no Linux build of that DLL, so every `"backend": "vector"` config in
`host_tools/` is unusable on the Pi as written. Vector's own driver downloads for the VN1600
family are Windows packages, and no aarch64 Linux driver — or SocketCAN exposure for the
VN1610 — appears in their public material. Treat Pi support as **not available** unless Vector
support says otherwise in writing; do not plan around it.

This is a better outcome than it looks. **Keep the VN1610 on the dev laptop** — it is
already what `bvt/benches/dev.json` describes (§3.3):

- The Pi bench runs unattended on socketcan; the VN1610 stays free for interactive debugging
  and trace work at the desk, instead of being tied up by every PR.
- Two benches on different transports is the forcing function that keeps `bvt/bench/bus.py`
  an honest abstraction. A transport seam with one implementation is a seam in name only.
- The desk bench stays usable when the Pi is torn down for the SPI-bridge project (§"Scope
  note").

### 1.3 CAN FD rules out most cheap CAN hardware

The bus is 500 kbit arbitration / 2 Mbit data with BRS (`can_echo_probe/config.json`). The
common **MCP2515 HATs are classic-CAN only and will not work** — this is the mistake to avoid.

Do **not** work around this by running the bench at classic-CAN rates: the nodes' bit timing is
compiled in, so a slower bench would be testing a build you don't ship. Software CAN on an
RP2040 (`can2040`) is likewise CAN 2.0B only — no FD.

Options for the Pi, cheapest first (SGD, approximate):

| Option | ~SGD | Notes |
|---|---|---|
| **STM32G431 "CANable 2.0" clone** | 15–40 | Cheapest real FD. **Firmware roulette** — many ship slcan (classic only) and must be reflashed; upstream `candleLight_fw` does *not* yet support G431, so you need the vendor's gs_usb-compatible build. Budget an evening, accept it may not work |
| **MCP2518FD HAT** (Waveshare 2-CH, Seeed, CANFDZeroHAT) | 40–70 | **Best value.** Mainline `mcp251xfd` driver, already in Raspberry Pi OS. Needs a device-tree overlay. SPI-attached → watch RX overruns under load. Two channels = tester + independent passive logger |
| **candleLight FD** (STM32G0B1) | ~90 | Genuinely supported by upstream `candleLight_fw`, isolated. The "just works" USB option |
| **PEAK PCAN-USB FD** | ~300 | Reference-grade `peak_usb`. Only if the bench becomes load-bearing |

Two buying criteria that decide whether it works at all:

- **40 MHz oscillator** on any MCP2518FD board. A 20 MHz part makes a clean 2 Mbit data phase
  with good sample points hard-to-impossible. The mainline driver reports the clock at probe —
  check `dmesg` for `c:40.00MHz`.
- **Kernel ≥ 5.18** for CAN FD over `gs_usb`. Raspberry Pi OS Bookworm is fine.

Two channels are genuinely useful: channel 1 is the tester/attacker node, channel 2 is a
passive logger whose trace is uploaded on failure. With a single channel you can still do
both, but the trace and the stimulus share a queue.

### 1.4 …but buy none of it yet — Stages 1–3 need no new hardware

Cost pressure exposes a sequencing error in this brief's first draft. Re-read §1.1: **the
dev laptop is already a mandatory part of the pipeline**, because MTB only builds on
x86/Windows. And it already has the VN1610. Which means:

> **A complete BVT bench exists today on hardware you already own.** Laptop = builder +
> tester + programmer. Zero purchase.

What the Pi adds is exactly one thing: **unattended operation** — PRs tested when the laptop
is off or busy. That is worth having, but it is not what you asked for. The stated pain is
*"every new feature, I re-test existing features and reflash by hand"*, and a laptop bench
removes all of that on day one.

So invert the staging: **build the bench on the laptop first, run it for a few weeks, buy the
Pi hardware only when the suite has proven it is stable enough to be worth automating
unattended.** The transport seam (§3) makes the migration a config file plus one `Bus`
implementation — that abstraction was already justified in §1.2, and this is the second time
it pays.

Deferring also de-risks the purchase: after a few weeks of real runs you will know whether you
need two channels, whether SPI RX overruns matter at your bus load, and whether the bench
warrants a PCAN at all. Buying now is buying before you know which of those is true.

Free power-cycling in the meantime: KitProg3 `XRES` via the debugger you already have. It's a
soft reset, not a power cycle — weaker recovery for a wedged board, but you're at the desk
anyway during Stages 1–2. The relay/hub purchase moves to Stage 3 with everything else.

---

## 2. Bench bill of materials

**Stages 1–3 buy nothing** — laptop + VN1610 + both kits, all owned (§1.4). The list below is
the **Stage 3** purchase, made only once the suite has earned it.

- Raspberry Pi 5 (owned) + PSU + storage. 64-bit Raspberry Pi OS.
- CAN FD interface per §1.3 (the VN1610 stays on the laptop — §1.2).
- **Per-port-switchable USB hub** for power cycling, driven by `uhubctl`. Both kits are
  KitProg3/USB-powered — verify each kit's power-select jumper actually takes USB, and check
  the hub model against uhubctl's compatibility list *before* buying (most hubs silently
  ignore per-port power commands).
  - Fallback: a relay HAT on the kits' barrel jacks, or KitProg3 `XRES` via the debugger.
    A relay is a *harder* reset than XRES and is what you want when firmware hangs.
- Wiring: CANH/CANL/GND from the Pi adapter to both kits.
  - **Exactly two 120 Ω terminations on the whole bus.** Three nodes now share it — check the
    kits' termination jumpers *and* the adapter's internal termination. Three terminations is
    the classic 3-node bench failure and shows up as random CRC/ack errors, not as an obvious
    fault.
- USB: Pi → KitProg3 on each kit (also the flashing path if §1.1(B) works).

- **UART console on both nodes — see §2.1.** No extra cables expected: KitProg3 also bridges a
  debug UART over the same USB link, so the host should see one serial port per kit (a `COM*`
  on Windows, `/dev/ttyACM*` on Linux). *Verify which SCB instance and pins each kit routes to
  the bridge* before designing around it.

### 2.1 The UART log (agreed — do it before the bench)

There is no `retarget-io` anywhere in either node today, so a remote BVT failure currently
leaves only a CAN trace. Adding UART is the right call, but three decisions in it are not
throwaway.

**(a) `printf` is not available to you.** `CLAUDE.md` rule 5 and `docs/coding-standard.md`
forbid `<stdio.h>` in production C. `cy_retarget_io` is a printf shim — dropping it in breaks
your own standard on the first line. Options:

| | Approach | Verdict |
|---|---|---|
| A | Thin byte logger — `log_puts(const char *)`, `log_hex_u32()` straight onto the SCB via PDL. ~40 lines, no varargs, no stdio, MISRA-clean | **Start here.** Human-readable during bring-up, when you need it most |
| B | Structured events — `log_evt(uint16_t id, uint32_t arg)`, fixed 8-byte records, decoded on the host from a generated ID table | **Where this should end up** — see (b) |
| C | `cy_retarget_io` + a documented MISRA deviation | Fastest, but you'd be deviating for convenience, which is the deviation you least want to defend in an interview |

Design A's call sites so B is a drop-in swap.

**(b) Structured logs turn your weakest tests into your strongest.** BVT tests 5 and 6
(replay, forgery) assert *"the actuator holds its last-known-good state"* — a **negative
observation**, which also passes if the node is simply dead, wedged, or never received the
frame. That is a test that cannot distinguish "rejected the attack correctly" from "wasn't
listening". A logged rejection with a reason code (`SECOC_REJECT_FRESHNESS`,
`SECOC_REJECT_MAC`) converts it into a positive assertion: *the node saw the frame, verified
it, and refused it for the right reason.* For a milestone whose entire point is security
properties, that difference is the difference between a test and a placebo.

So: logs are evidence **except** where a structured event is the only positive proof of a
rejection — those events are part of the contract and may be asserted. Keep free-text logs out
of the gate entirely; they change too often to assert against.

**(c) Two UART bridges, four cores.** One bridge per kit, but CM0+ on both nodes has things
worth saying (MAC verify results, key-store state). Do not invent a second cross-core channel:

- **Node A** — CM4 owns the UART; CM0+ log records ride the existing crypto-service IPC
  mailbox, drained by CM4.
- **Node B** — CM7 owns the UART; CM0+ goes through the existing IPC mailbox, which already
  lives in the **MPU non-cacheable region per ADR-0018 D6**. A naively-added shared log buffer
  on Node B would walk straight into the CM7 L1 cache-coherency problem that ADR already
  solved. Reuse the solved path.

**(d) Host-side plumbing — identify boards by serial number, not by port.** COM numbering on
Windows (like `ttyACM*` on Linux) reshuffles across reboots and hotplugs, and **a bench that
flashes the wrong board is worse than no bench.** With two identical-looking KitProg3s this is
a when, not an if.

Resolve both kits by their **KitProg3 USB serial number** at bench startup and map that to the
node role in `bvt/benches/dev.json` — `pyserial`'s `list_ports` exposes it on Windows, and the
same identifier is what OpenOCD's `cmsis_dap_serial` wants for flashing, so one lookup pins
both the log reader and the programmer to the right board. (On Linux later, `udev` rules give
the same guarantee as stable symlinks.) Fail the run loudly if either serial is missing rather
than falling back to "first port found".

The log reader must be **attached before the reset**, or every run loses the boot banner —
exactly the part you need when boot is what broke.

---

## 3. Harness architecture

Same discipline as the firmware: strict layers, hardware behind interfaces, the logic
host-testable with no bench attached.

```
bvt/tests/            pytest suites — name no hardware, no CAN IDs from raw hex
      │
bvt/bench/bench.py    Bench facade: power_cycle(), flash(node, img), expect(msg, timeout),
      │               inject(frame), capture()  ← the seam tests depend on
      │
      ├── bvt/bench/power.py      uhubctl | relay | XRES        (interface + impls)
      ├── bvt/bench/programmer.py uds_flash | pyocd | openocd   (interface + impls)
      ├── bvt/bench/bus.py        python-can: socketcan | vector | virtual
      │
bvt/proto/            protocol clients — isotp, uds, secoc frame build/verify, body_msgs
                      *** pure logic, no I/O, unit-tested on any machine ***
```

### 3.1 The refactor this depends on (do it first, it pays for itself)

`uds_flash.py`, `uds_seam4_probe`, `uds_seam5_probe`, and `isotp_bringup_probe` **each
re-implement the simplified ISO-TP PCI layout and the UDS SID constants**. Four copies of a
wire format that is about to be exercised on every PR is a latent bug farm — and worse, a
copy can drift from `shared/diag/include/*.h` and make BVT lie.

Extract once into `bvt/proto/` (or `host_tools/lib/`), have the probes import it, and unit-test
it. The probes keep their CLIs and README value; they just stop owning the protocol.

Stretch, worth considering: generate the SID/PCI constants from the C headers so a change in
`shared/diag/include/` can't silently desync the tester.

### 3.2 The fake bench (mirrors ADR-0001's host-testable core)

`bvt/bench/fake.py` implements the `Bench` interface over a `vcan0` virtual interface or an
in-process loopback, with a scriptable stand-in for each node. Consequence: **the BVT suite's
own logic is testable in cloud CI with zero hardware**, and a broken harness fails in T0
rather than burning bench time and looking like a firmware regression. Same argument as the
`*_port_fake.c` files — and the same payoff.

### 3.3 Config, not code

One JSON per bench, following the existing `config.json` convention:

- `bvt/benches/dev.json` — Vector VN1610, dev laptop, no power control (manual bench)
- `bvt/benches/pi.json` — socketcan, uhubctl power, pyocd/uds_flash programmer
- `bvt/benches/fake.json` — vcan/loopback, used by T0

`pytest --bench=pi`. The same suite runs on the desk bench during development, which is what
stops the BVT rotting between hardware sessions.

---

## 4. What the BVT actually asserts

Ordered so the cheapest, most diagnostic test fails first. Each test starts from a defined
state: **power cycle → wait for liveness**.

| # | Test | Proves | Milestone |
|---|---|---|---|
| 0 | **Flash Node A app over UDS** — session, unlock, erase, download, CRC/signature check, ECUReset | ISO-TP, UDS, flash driver, ECDSA verify, FBL jump | M3, M4 |
| 1 | **Liveness** — expected periodic frame on the bus within N ms of reset, both nodes | FBL verified + jumped, FreeRTOS scheduling, CANFD up, clocks | M1, M2 |
| 2 | **Gateway function** — inject `0x200` sensor report, assert the `bodyctl` courtesy-light output | app logic on target matches the host unit tests | M2 |
| 3 | **Secure-boot negative** — flash an image with one flipped signature byte; FBL must **refuse to jump** and stay in FBL; then restore | the security property, not just the happy path | M4 |
| 4 | **SecOC accept** — Node A → Node B authenticated command; actuator state changes, observed via its status frame | CMAC offload, cross-node key agreement, freshness | M5 |
| 5 | **SecOC replay** — tester re-injects a captured valid frame; actuator holds last-known-good state **and logs `SECOC_REJECT_FRESHNESS`** (§2.1b) | the freshness counter actually works | M5 |
| 6 | **SecOC forgery** — valid payload, wrong MAC; state held **and `SECOC_REJECT_MAC` logged** | verification is real, not just absent | M5 |
| 7 | **Bus health** — no error frames, no bus-off, no fault frame across the whole run | nothing is silently degrading | all |

Tests 3–6 are the ones worth the bench existing: they are security *properties*, they are
tedious by hand, and they are exactly what quietly breaks when unrelated code moves.

**Test 7 is an assertion, not a log line.** A run where everything passed but the bus threw
error frames is a failing run.

### 4.1 Two traps specific to this design

- **Freshness counters are persistent.** SecOC freshness lives in `eeprom_emu`, so a power
  cycle does *not* reset it. Tests 4–6 mutate durable state, which means BVT is order-coupled
  and not idempotent across runs. Decide deliberately: either the suite always begins with the
  resync path (ADR-0021 D5) as its first SecOC action — which makes resync itself a tested
  property, the better option — or expose a test-only reset. Do not let this be discovered as
  a flaky test at 2 a.m.
- **Flash endurance is a consumable.** Code flash on these parts is spec'd in the low
  thousands of erase cycles. Test 0 + test 3 = 3 flashes per PR; at 10 PRs/day that is a real
  budget. Mitigations: cache the last-flashed image digest on the bench and **skip the reflash
  when the image is byte-identical**; keep test 3 in T2 (nightly) rather than T1; count and
  log cumulative flash cycles per board so the number is visible before it's a mystery.

### 4.2 Node B has no FBL

Node B can only be flashed by debugger. If the §1.1(B) spike fails, the honest fallback is:
BVT flashes Node A only, pins Node B to a known image, and **fails loudly** with "Node B
sources changed, image is stale — reflash manually" when a PR touches `node_b_actuator/`.
A stale-image false-pass is far worse than a blocked PR.

---

## 5. CI wiring

**One self-hosted runner** — `traveo-bench` on the dev laptop, labels
`self-hosted, windows, traveo-bench`. Build and test collapse into a single job, because the
machine that has MTB is the machine that has the VN1610:

```
host-ci (GitHub-hosted)  ──▶  bvt (traveo-bench, one job: build → flash → test)
   make test / lint
```

That collapse removes the artifact upload/download hop entirely. If the build ever moves to a
containerized x86 GitHub-hosted runner (`getlibs` from the committed `deps/*.mtb`, cached), the
two-job split comes back — worthwhile later, not on the critical path.

Non-obvious details that matter:

- **`needs: host-build-test`.** Never burn bench time on code that fails the host suite.
- **`concurrency: { group: traveo-bench, cancel-in-progress: false }`.** One bench, many PRs —
  queue them. **Do not cancel in progress**: a cancelled job mid-flash leaves a board in an
  unknown state, and the next run inherits it.
- **Path filter + label override.** Auto-run on `node_*/**`, `shared/**`, `host_tools/**`; a
  `bvt` label forces a run otherwise. Docs-only PRs shouldn't touch hardware.
- **Fork PRs must not reach the self-hosted runner.**
  `if: github.event.pull_request.head.repo.full_name == github.repository`. A self-hosted
  runner executes PR code on your machine — this is the one security-relevant line in the file,
  and it matters more on a laptop than it would on a dedicated Pi.
- **`timeout-minutes: 20` + an `always()` teardown** that resets both boards and releases the
  CAN channel. A hung job must not hold the bench — or the VN1610 — hostage.
- **`if: always()` artifact upload**: JUnit XML, the CAN trace (`.asc`), and the UART logs
  (§2.1). Even at the desk, the trace is what turns a red run into a diagnosis.
- **Do not make BVT a required check until it has been green for ~2 weeks.** A flaky
  hardware gate that blocks merges gets disabled permanently within a month; a flaky advisory
  gate gets fixed. Earn the promotion.

### 5.1 Laptop-specific gotchas (these are what will actually bite)

- **Sleep is the #1 failure mode.** Lid closed → runner offline → queued PR jobs hang until
  they time out. Either set "never sleep while plugged in" *and* disable USB selective suspend
  (which will otherwise drop the VN1610 and the KitProg3s mid-run), or accept it and keep BVT
  **label-triggered and non-required** so a sleeping laptop never blocks a merge. Pick
  deliberately — this single choice decides whether the gate is trustworthy.
- **Install the runner as a Windows service** so it survives logout and reboot
  (`config.cmd --runasservice`). The catch: it then runs under a service account with a
  *different* `PATH` — MTB, `arm-none-eabi-gcc`, Git Bash, and Python must all resolve there,
  not just in your interactive shell. Set `CY_TOOLS_PATHS` explicitly in the job env; the
  `zz_build_*.sh` scripts already forward it as `CY_TOOLS_DIR`.
- **`shell: bash` works on Windows runners** (Git Bash ships with the runner), so the existing
  `zz_build_*.sh` scripts run unmodified. This is why the build step is nearly free.
- **The signing key must be provisioned, never fetched.** `ec_p256_dev_private.pem` is
  gitignored and must exist on the runner *outside* the workspace, referenced by
  `FBL_SIGN_KEY` as an absolute path. Do not add it to the repo, do not put it in a GitHub
  secret and write it to disk in the job — a self-hosted runner has a persistent filesystem, so
  provision it once by hand and leave it there. Mask any path echo.
- **`make program` is a trap in signing mode** — your own script says so: it flashes the
  *unsigned* image, which a SHA-256 FBL then rejects. The job must build, then flash Node A's
  app via `uds_flash.py`. `make program` is only for the FBL and Node B.
- **The VN1610 is exclusive.** One process owns a Vector channel at a time — if CANalyzer or a
  stray probe is open, the bus won't open. Add a preflight check that fails fast with *"VN1610
  channel busy — close the other tool"*, not a 30-second timeout. Same for the Vector Hardware
  Config app-name mapping (your `can_echo_probe` README already calls this the usual first-run
  gotcha).
- **You are sharing the bench with yourself.** Unplugging the VN1610 or opening CANalyzer
  mid-run produces a red BVT that is not a firmware bug. Another argument for label-triggered
  runs at first — you decide when the bench is yours.
- **Reset without a relay:** UDS `ECUReset` (0x11, already implemented — `uds_flash.py` ends
  with it) for the normal path, KitProg3 `XRES` via OpenOCD when the app is unresponsive. A
  true power cycle only matters for a board wedged past `XRES`, which is rare and which you are
  sitting next to anyway.

---

## 6. Staging

Each stage is independently useful; stop at any point and you're still ahead.

**Stages 0–3 cost nothing** (§1.4). Money is spent at Stage 4, or never.

- **Stage 0 — UART logging (§2.1).** Approach-A logger on all four cores, CM0+ routed via the
  existing IPC mailboxes. Useful immediately at the desk, independent of everything else, and
  the piece that makes every later failure debuggable — so it goes first even though it isn't
  the bench. *(Verify which SCB/pins each kit routes to the KitProg3 UART bridge.)*
- **Stage 1 — Harness, no hardware.** `bvt/proto/` extraction (§3.1), `Bench` interface, fake
  bench, tests 1/2/4/5/6 written against the fake and green in T0. **The whole suite exists and
  is CI'd before any bench is wired.**
- **Stage 2 — Dev-box bench, manual trigger.** `bvt/benches/dev.json`: VN1610, MTB's OpenOCD
  for flash + `XRES`. Run by hand until green twice in a row; fix what the real bus disagrees
  with. **This is the stage that eliminates the manual re-testing** — everything after it is
  about *unattendedness*, not coverage.
- **Stage 3 — CI on the laptop.** Register it as a self-hosted runner, add the `fw-build` +
  `bvt` jobs, advisory (non-required) status. Add test 0 (reflash over UDS) and test 3
  (secure-boot negative). PRs now gate themselves whenever the machine is on.
- **Stage 4 — Pi migration (the purchase).** *Only if Stage 3 has been stable and the "laptop
  must be on" constraint actually bites.* Now run the §1.1 spikes — pyocd on aarch64, adapter
  at 500k/2M, `uhubctl` — buy per §1.3/§2, add `bvt/benches/pi.json`, move the runner label.
  Everything above this line is unchanged by the move; that is the point of the seam.
- **Stage 5 — Promote + extend.** Make BVT required. Split T2 nightly: full attack matrix, the
  M3 reprogramming timing measurement, a soak run.

**Deliberately not now:** no OTA, no HIL fault injection on the physical layer (no shorting
CANH/CANL, no supply-voltage ramps), no test-report web UI, no LLM anywhere in the gate.

## 7. Deliverables

1. UART logging on both nodes (§2.1) + the KitProg3-bridge pin finding, recorded in the
   findings log. *(The §1.1 pyocd spike moves to Stage 4 — it is not a prerequisite.)*
2. **ADR-0022 — hardware BVT bench.** The layering, the `Bench` seam, the flash-over-UDS-vs-
   debugger choice, the "reflash *is* the M3/M4 test" argument, and one alternative per
   boundary. Plus the freshness-persistence decision from §4.1 — that one is a genuine
   architectural call, not a config detail.
2b. **ADR-0023 — target logging channel.** Free-text vs. structured events, the stdio/MISRA
   constraint, and the rule for when a log line may be asserted (§2.1b). Small ADR, but it
   pins down an interface that four cores and the whole BVT suite will depend on.
3. `bvt/` skeleton: interfaces, fake bench, failing tests. Protocol extraction from
   `host_tools/` with the probes migrated onto it.
4. `bvt/benches/*.json` + a `make bvt` target; `bvt-fake` folded into the existing T0 CI job.
5. `.github/workflows/bvt.yml` per §5.
6. A short `docs/` writeup + bench photo. *"My PRs are gated by a hardware-in-the-loop test
   that reflashes two ECUs over UDS and tries to spoof the CAN bus"* is a strong portfolio
   line, and it lands better than the bench itself.

## 8. Watch-fors

- **Flakiness is the whole ballgame.** One flaky hardware test destroys trust in all of them.
  Any test that fails intermittently gets *deleted or fixed* within a day — never retried into
  submission. Budget generous timeouts, but assert exact behaviour.
- **Bus termination** (§2) — three nodes, two terminations. Check it before debugging firmware.
- **Order coupling via persistent state** (§4.1) — freshness counters and any `eeprom_emu`
  content survive power cycles. Tests must either not care or explicitly establish state.
- **A hung target must be recoverable without hands.** If the only recovery from a wedged
  board is a human pressing reset, the bench will be down every time you're not at the desk.
  This is what the relay is for.
- **The tester can lie.** `bvt/proto/` duplicating a wire format that drifts from
  `shared/diag/` produces confident false passes — the worst possible failure mode for a gate.
  Single source, unit-tested, ideally generated.
- **Don't let BVT grow.** Every added test costs bench minutes on every PR forever. New checks
  default to T2; promotion to T1 is a deliberate decision.

## Findings log

*(Stage-0 spike results, bench quirks, and every flaky-test postmortem go here — same
convention as the `M*-bringup_log.md` files.)*
