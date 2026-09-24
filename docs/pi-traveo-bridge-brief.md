# Pi 5 ↔ Traveo II bridge — Linux-credibility project (architecture-first brief)

> **Purpose:** the Linux/QNX-visibility piece for the Bosch ACS application (see
> `bosch-acs-requirements-mapping.md`). It turns 🟡 "Linux C/C++ / IPC / device-tree" into
> ✅ artifacts on real hardware, and gives an on-ramp to the QNX IPC comparison.
> **Role for Claude Code:** act as an **embedded-Linux platform architect** — design the
> layered inter-unit link first (interfaces, dependency direction, host-testable seams),
> tradeoffs + one alternative per boundary, THEN code. Same discipline as the Traveo ADRs.

## The one-line pitch (this is what an interviewer hears)

*"Two compute 'units' — a Linux host (Pi 5) and an automotive MCU (Traveo II) — exchanging
messages over a low-speed link with a small, framed IPC-style protocol, designed as layered
platform software with the transport behind a driver seam."* That sentence maps almost verbatim
to the ACS role's **"communication between units — intra-die IPC, external low-speed interfaces."**

## Design decisions (already made — the b-a-c shape)

- **(b) Chiplet-style inter-unit link**, not a plain flash tool. The Pi and the Traveo are modeled
  as two *units*; the deliverable is a **message-passing protocol between units**, which is the
  role's headline theme. (It can still *carry* diagnostic/UDS payloads — see reuse below — but the
  framing is IPC-between-units, not "PC tool talks to ECU".)
- **(a) SPI** as the physical link. Fast, kernel-driver-friendly, and the closest low-speed bus to
  the "intra-die-ish" feel. Pi 5 as SPI **controller**, Traveo as SPI **peripheral**. (CAN stays
  available as a second transport later — see stretch — which is where M3/M4 reuse plugs in.)
- **(c) Real Linux driver + device-tree overlay as the depth goal.** Start in userspace
  (`spidev`) to get the protocol working fast, then graduate the Traveo-facing side to a **custom
  device-tree overlay + a kernel-space driver** as the Linux-C-depth proof. Staged so there's a
  demonstrable result early and depth if time allows.
- **QNX on-ramp (deliberate):** the protocol/IPC layer is written portable so the *same* message
  API can later sit on **QNX message-passing** — making this the vehicle for the
  QNX-vs-Linux-vs-FreeRTOS IPC comparison essay. Design the API now so QNX is a port, not a rewrite.

## Architecture to design FIRST (layered, like the diagnostic stack)

Strict one-directional dependencies (upper depends on lower via interfaces; never sideways):

1. **Physical / transport (SPI).** Move bytes. Pi side: `spidev` first → kernel driver later.
   Traveo side: SPI-peripheral driver behind its HAL (reuse the M-series port pattern).
2. **Framing / link layer.** Turn a byte stream into delimited, length-checked, CRC'd **frames**
   (start/len/payload/CRC; handle partial reads, resync). Knows nothing about message *meaning*.
3. **Message / IPC layer.** A small **message-passing API** over frames: typed messages, request/
   response + notification, IDs, a sequence/ack scheme. This is the "IPC between units" core and the
   **QNX-portable** boundary — define it as a clean API, transport-agnostic.
4. **Application / services.** Demo services over the IPC: a **heartbeat/health** exchange, a
   **read/write "unit register/state"** service, and (reuse) a **diagnostic passthrough** that
   tunnels a UDS/`.noinit`-style request to the Traveo — so M3/M4 work is *carried* by the bridge,
   not duplicated.

### Seams that must exist from day one
- **Transport interface** — framing layer depends on a `send_bytes/recv_bytes` interface; `spidev`
  and (later) the kernel driver are two implementations. Also lets the framing + message layers be
  **host-tested on the Pi with a loopback/fake transport, no MCU needed.**
- **Message API = the QNX-portable boundary.** Services call the message API, never touch SPI or
  frames. Swapping Linux→QNX under it changes the transport/IPC impl, not the services.
- **Traveo side mirrors the same layering** (SPI HAL ← framing ← message ← service), reusing the
  host-testable-core discipline from the Traveo project (ADR-0001): framing + message logic are
  pure and unit-tested; only SPI I/O is behind the port.

### Patterns to name (interview-defensible)
Layered architecture w/ enforced dependency direction; a **transport Strategy** (spidev vs kernel
driver vs — later — CAN/QNX); framing as a **state machine** (host-testable); message dispatch as
**command/handler**. Same architecture-mindset vocabulary as the Traveo ADRs — consistency across
both projects is itself a signal.

## Host/target/2-sides test split

| Host-testable (pure logic, both sides) | Target/OS-specific (behind seams) |
|---|---|
| Frame encode/decode, CRC, resync/partial-read state machine | Pi: `spidev` ioctl / the kernel driver |
| Message encode/decode, seq/ack, dispatch | Traveo: SPI-peripheral driver (HAL port) |
| Service logic (heartbeat, register r/w, diag-passthrough routing) | Device-tree overlay; kernel module glue |

Pi-side logic: C/C++ with unit tests (gtest/Unity/CTest — pick one, keep it CI'd). Traveo-side:
reuse the existing Unity + host-fake setup. A **loopback fake transport** lets the whole protocol
be tested with zero hardware, then validated on the real SPI link.

## Scope (keep it staged; each stage is demoable + a blog beat)

- **Stage 1 — protocol over spidev (the fast win).** Pi (`spidev`, C/C++) ↔ Traveo (SPI peripheral):
  framing + message layer + a heartbeat and a register-read/write service. Host-tested via loopback,
  then real link. *This alone closes the Linux-C/C++ + IPC + SPI + device-tree(basic) gaps.*
- **Stage 2 — kernel depth.** A **device-tree overlay** for the link + a small **Linux kernel-space
  driver** (chardev or a thin protocol driver) replacing raw spidev on the Pi side. The C-in-kernel
  proof piece.
- **Stage 3 — diagnostic reuse (ties the portfolios together).** Tunnel a UDS/reprogramming or
  `.noinit`-request payload from the Pi through the bridge to the Traveo — the Pi becomes the Linux
  host for your M3/M4 flow. One repo now spans Linux platform + automotive protocol.
- **Stretch — CAN transport** (a second Strategy under the message API) and/or the **QNX port** of
  the message/IPC layer (feeds the IPC-comparison essay).

**Deliberately deferred / non-goals:** no CI/CD server on the Pi (off-axis for this role — noted
before); no chiplet *hardware* (concept-level only); QNX port is stretch, not Stage-1 scope.

## Deliverables (in order)

1. **Layered architecture design** — the 4 layers, the transport interface, the QNX-portable message
   API boundary, tradeoffs + one alternative per boundary. **Stop; agree before code.**
2. ADR(s): the link architecture; the transport-Strategy seam (spidev→driver→CAN→QNX); the framing
   protocol spec (frame format, CRC, resync). A short **protocol spec doc** (this is "API definitions
   / integration guides" — a posting bullet).
3. Skeletons + interfaces both sides; loopback fake transport; failing tests for framing + message
   layers.
4. Implement Stage 1 against tests; green on Pi CI (GitHub Actions building the Pi-side C/C++ +
   running the host-testable logic) and the Traveo Unity suite. Then real-hardware bring-up.
5. Stage 2 (device-tree overlay + kernel driver), then Stage 3 (diag passthrough).

Start with step 1 only.

## Watch-fors
- **SPI controller/peripheral timing & framing** — partial reads, byte alignment, resync after a
  glitch; get the framing state machine right in host tests before trusting the wire.
- **Don't let SPI/ioctl leak into the message or service layers** — that leak is the layering bug
  (same rule as the diagnostic stack). Services see typed messages only.
- **Keep the message API transport- and OS-agnostic** — or the QNX port becomes a rewrite and the
  CAN transport won't slot in.
- **Device-tree overlay correctness** (Stage 2) — a wrong overlay silently fails to bind; validate
  the node/driver match deliberately.
- **Kernel vs userspace scope creep** — Stage 1 in userspace *first*; don't start in the kernel and
  stall. A working userspace demo beats a half-written driver.

## Why this is the right Linux-credibility piece (recap for the CV/interview)
One project, on hardware you already have, that produces real artifacts for: **embedded Linux
C/C++**, **device tree + a kernel driver**, **inter-unit IPC** (the role's core theme), **SPI**
(and CAN via reuse), **architecture docs + a protocol/API spec**, a **QNX on-ramp**, and it
**connects to your automotive M3/M4 work** so the two halves of your profile tell one story.
