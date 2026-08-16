# Handoff — M5 Step 7: SecOC events, then resume SecOC

**Date:** 2026-08-16 · **Branch:** `m5-secoc` · **Last commit:** `2183bd4`
**Repo:** `C:\00_Projects\00_Portfolio\00_TraveoBody\traveo-body-domain`

> **Task for the new chat:** wire the SecOC verdict events into the receive path on
> **Node B's CM7**, then hand back to the paused SecOC milestone. The logging channel that
> makes those events possible is **done and proven on silicon on both nodes** — do not
> redesign it.

## How to work in this project (read first)

- **Role/rhythm:** automotive diagnostic-stack / security architect. First deliverable of any
  seam is *design* with tradeoffs + at least one alternative per boundary (blog/interview
  material). Rhythm: **discuss → agree → ADR → failing host test → implement → bring-up.**
- **Challenge decisions, don't just agree.** Give a recommendation, not a survey. The user
  pushes back well and has twice improved the design by doing so (see "Decisions the user
  changed" below) — expect that and engage with it properly.
- **Hardware facts live in `CLAUDE.md`.** Don't guess them. Flag register-level claims as
  "verify in TRM" — the TRM PDF is local at `docs/references/TraveoT2G_BodyControlEntry_TRM.pdf`
  and readable via `pdftotext -layout`. Use it rather than guessing; it has already settled
  three questions this milestone.
- **MISRA C:2012** for target/production C (`docs/coding-standard.md`); host tests are exempt.
  Hardware-independent modules must not include vendor headers (ADR-0001).
- **Workflow split** (`docs/workflow.md`): Claude Code owns contracts/ADRs/review; Copilot fills
  bodies. The user normally runs the tests — **but has granted permission to run `make lint`
  and the host suites directly**, which has been valuable and should continue.
- **Traceability:** every ADR decision gets a `REQ-*` in `docs/requirements/`, and code carries
  `@impl` / `@test` tags (`docs/traceability-convention.md`). Do not skip this — it was missed
  once and the user caught it.

## Build / test commands

```bash
# Host suites — the AI Bash tool must set TMP or gcc fails with "Cannot create
# temporary file in C:\WINDOWS". `make test` does NOT propagate it; compile directly.
export TMP='C:\Users\ADMINL~1\AppData\Local\Temp\claude' TEMP="$TMP"
gcc -std=c17 -Wall -Wextra -Werror -O1 -g -pipe -Ivendor/unity/src \
    -Ishared/log/include -Ishared/log/tests vendor/unity/src/unity.c \
    shared/log/src/log.c shared/log/tests/test_log.c shared/log/tests/log_port_fake.c \
    -o build/test_log && ./build/test_log

make lint                                  # works fine, no TMP issue
python -m pytest host_tools -q
python host_tools/gen_log_events.py --check # generated tables current?
make -n test | bash                        # full Unity suite, TMP propagated
```

Firmware (user's own MTB shell — the AI cannot build these):
```bash
./zz_build_gateway_fbl.sh program            # Node A FBL
./zz_build_gateway_app.sh program            # Node A app
./zz_build_actuator_app.sh program cm7       # Node B CM7
python host_tools/logview/logview.py --port COM7 --raw capture.bin
```

## State: the logging channel is DONE

Four commits, all silicon-verified: `57b2bd4`, `8b47e43`, `2989e9f`, `58c7518`, `2183bd4`.

| Piece | Where | Status |
|---|---|---|
| Core + 30 Unity tests | `shared/log/` (`tb_log.h`) | ✅ |
| Node A app port + drain task | `node_a_gateway/app/src/port_log.c`, `log_task.c` | ✅ silicon |
| Node A FBL port + boot/jump/reject | `node_a_gateway/bootloader/proj_cm4/src/` | ✅ silicon |
| Node B CM7 port + drain + liveness | `node_b_actuator/proj_cm7/src/` | ✅ silicon |
| Generator, decoder, live viewer, 29 pytest | `host_tools/` | ✅ CI-gated |

**Design:** ADR-0023 · **Requirements:** `REQ-LOG-001..015` in `docs/requirements/logging.md` ·
**Findings log:** `docs/briefs/M5-bringup-plan.md` (8 entries — read #4–#8, they are the
non-obvious ones).

Key properties to rely on, not rediscover:
- 16-byte binary records, CRC-16/CCITT-FALSE, `0xA5` sync **outside ASCII** so text and records
  share one wire. Baud 1 Mbps, oversample searched at runtime.
- **Contract vs diagnostic ID ranges** (`< 0x1000` = assertable). This is the rule that makes
  "may a test assert on this?" mechanical — respect it.
- `events.csv` is the single source for `log_events.h` **and** the Python decoder; CI fails on
  stale generated files. **Never hand-edit the generated files.**
- Output is cyclic (20 ms drain), `ts_ms` sampled at production time, `LOG_DRAIN_LATENCY_MS`=50
  is the published bound an absence-assertion must wait out.

## Step 7 — the actual task

Emit the SecOC verdict events on the receive path. **They belong on Node B's CM7**, because
Node B is the actuator (receiver) and per **ADR-0021 D1** the MAC truncation and constant-time
compare happen in `shared/secoc` on the *app* core — the CM0+ is a generic CMAC oracle that
never learns the verdict.

Already defined in `shared/log/events.csv` (contract range — tests may assert on them):

| ID | Event | arg0 | arg1 |
|---|---|---|---|
| `0x0101` | `LOG_EVT_SECOC_ACCEPT` | `can_id` | `freshness_ctr` |
| `0x0102` | `LOG_EVT_SECOC_REJECT_MAC` | `can_id` | — |
| `0x0103` | `LOG_EVT_SECOC_REJECT_FRESHNESS` | `can_id` | `rx_ctr` |
| `0x0104` | `LOG_EVT_SECOC_RESYNC` | `can_id` | `new_epoch` |

**Why this matters (the whole reason the logger jumped the queue):** the BVT's replay and
forgery tests assert the actuator *held its last-known-good state* — a negative observation that
passes identically if the node is dead, wedged, or never listening. A logged rejection with a
reason turns that into a positive assertion. See ADR-0023 Context and `REQ-LOG-009`.

**Constraints:**
- `REQ-LOG-009` requires emission **on the verify path itself**, after the verdict, before or
  with the drop. These are the observable form of the per-reason drop counters already required
  by **`REQ-SECOC-001`** — the two must not diverge.
- The verify path is per-frame. `log_evt()` is bounded and non-blocking by design (ADR-0023 D7),
  but do not add anything else there.
- `shared/secoc` must stay host-testable and free of vendor headers (ADR-0001). Decide
  deliberately whether the emission lives inside `shared/secoc` (needs `tb_log.h`, which is
  vendor-free — fine) or in the Node B call site. Prefer inside, so both nodes get it.

**Suggested order:** agree the placement → add `@impl REQ-LOG-009` tags + host test in
`test_secoc` asserting the right event fires per verdict → implement → bring up on the bench
with `logview` on Node B.

## After Step 7: resume SecOC

M5 SecOC has been **paused since 2026-08-01** for the logger. ⚠️ **The passing `secoc*` and
`crypto_mac` host suites do NOT mean SecOC is done** — the user has said so explicitly. The
Makefile comments calling them "stubs => red" are stale but are a deliberate work-in-progress
marker: **leave them alone, and do not offer to fix them again.**

Remaining SecOC work is in `docs/briefs/M5-bringup-plan.md` Stages 2–7 (AES-CMAC KAT per core,
cross-core MAC offload, cross-node key agreement, real bus, the security demonstrations,
resync). Uncommitted in-progress files exist in `node_b_actuator/proj_cm7/src/`
(`actuator_fsm.*`, `secoc_app.*`, `port_crypto.*`, `can_task.c`) — deliberately kept out of the
logging commits.

## Decisions the user changed (do not re-litigate)

- **CM0+ does not log.** Proposed a per-core shared-RAM ring; the user pushed back on security
  grounds and was right — no contract event originates on the security core, so it was TCB
  surface for nothing. ADR-0023 D4 revised, ring deleted. This also removed the MPU
  non-cacheable step on Node B and retired the shared-TCPWM-time-base follow-up.
- **Liveness is a flat 5 s**, first emission immediate (a ramp was proposed and dropped).
- **BVT bench runs on the dev laptop with the existing VN1610**, not a Pi — Stage 4 purchase
  deferred until the suite proves itself (`docs/briefs/BVT-bench-brief.md` §1.4).

## Traps this milestone actually hit

1. **An empty ring is not an empty UART.** `PutArray` reports bytes into the TX FIFO, not onto
   the wire; the next SCB re-init discards them. Cost: every FBL record, presenting as a mangled
   banner that looked like a baud fault. Hence `log_port_sink_idle()` and the two-phase flush.
2. **A bounded wait needs a bound that doesn't assume a clock.** `deinit_for_jump()` stops
   SysTick; a time-only bound would hang the boot path. Hence `LOG_FLUSH_MAX_STALL`.
3. **A restart is not record loss.** `log_init()` zeroes the sequence counter, so every handover
   and power cycle looked like dropped records — which under `REQ-LOG-007` fails a *good* run.
   Hence the decoder resetting on `LOG_EVT_BOOT`.
4. **A generic header name collides with the vendor SDK.** `log.h` vs the Ethernet driver's.
   Hence `tb_log.h`. `log_port.h`/`log_types.h`/`log_events.h` are still unprefixed — no
   collision today, worth prefixing before the next middleware.
5. **One hardcoded UART oversample can't serve two clock trees** (80 MHz vs 100 MHz).

Open, low priority: `LOG_BUILD_ID` still defaults to `nobuildid` — wire
`$(shell git rev-parse --short HEAD)` in each Makefile when the BVT starts reflashing between
tests (M5 finding #2).
