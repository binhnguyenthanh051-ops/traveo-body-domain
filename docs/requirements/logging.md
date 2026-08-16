# Logging requirements — `REQ-LOG-001..014`

Normative requirements for the target logging channel. Each traces **up** to an ADR-0023
sub-decision (the *why*) and **down** to the `@impl`/`@test` tags that prove it
(`docs/traceability-convention.md`). "shall" is normative; a requirement with no `@test` on disk
is a coverage gap the `checks/` pipeline flags.

Design context: `ADR-0023` (this channel), `docs/briefs/BVT-bench-brief.md` (the consumer).
Related: `ADR-0021` D1 (the SecOC verdict is decided on the app core — why the security core
does not log), `ADR-0016` D2/D5 (the `ERROR` verdict that reports M0+ health instead),
`ADR-0003` (MISRA), `ADR-0001` (host-testable core).

**Why this channel is normative at all.** For the SecOC rejection paths it is not telemetry —
it is the *only positive evidence* that a node saw a frame, verified it, and refused it. The
replay and forgery tests otherwise assert a bare negative ("the actuator held state"), which
passes identically when the node is dead. `REQ-SECOC-001` already requires a **per-reason drop
counter**; `REQ-LOG-009` is what makes that counter externally observable, and the two must not
drift apart.

## Coverage matrix

| ID | Requirement (short) | Traces | Verified by |
|---|---|---|---|
| REQ-LOG-001 | Producer emits fixed-size binary records; no `stdio`/varargs/formatting on target | D1 | `test_log` |
| REQ-LOG-002 | Record layout, LE fields, CRC-16/CCITT-FALSE, 16 B, byte-offset encoded | D2 | `test_log` |
| REQ-LOG-003 | `LOG_SYNC` outside ASCII; text runs pass through unescaped and are never assertable | D3, D6 | `test_log`, `test_log_decode` |
| REQ-LOG-004 | One SPSC ring **per image**, image-private RAM; the security core does not log | D4 | `test_log` + `@design-only` |
| REQ-LOG-005 | `log_evt()` is bounded, non-blocking, ISR-safe; critical section covers reservation only | D7 | `test_log` |
| REQ-LOG-006 | Overflow drops the **newest**, counts it, and emits `LOG_EVT_OVERFLOW`; `seq` gaps detect loss independently | D5 | `test_log` |
| REQ-LOG-007 | A consumer shall treat any overflow within a window as **failing** that window | D5 | `test_bvt_harness` |
| REQ-LOG-008 | Contract IDs (`< 0x1000`) are assertable and breaking to change; diagnostic IDs are never asserted | D6 | `test_log_events` |
| REQ-LOG-009 | SecOC accept/reject events emitted with reason + CAN ID, on the verify path | D6 | `test_secoc`, BVT tests 4–6 |
| REQ-LOG-010 | `log_panic()` bypasses the ring with a blocking, interrupt-free write | D7 | `test_log` |
| REQ-LOG-011 | Logic host-testable with plain GCC; all hardware behind `log_port_*` | D8 | `test_log` |
| REQ-LOG-012 | One generated event registry; stale generated files fail CI | D9 | `checks/` + `test_log_events` |
| REQ-LOG-013 | C encoder and host decoder round-trip identically | D8 | `test_log_decode` |
| REQ-LOG-014 | Drain latency is bounded and published, so absence-assertions can settle | D10 | `test_log`, `test_bvt_harness` |

## Requirements

### REQ-LOG-001 — structured records, no formatting on target
The producer shall emit fixed-size binary records and shall perform **no string formatting**.
Production logging code shall not include `<stdio.h>`, use varargs, or accept a format string.
All rendering shall occur on the host. *(ADR-0023 D1; ADR-0003.)*

### REQ-LOG-002 — record layout
A record shall be exactly `LOG_REC_SIZE` (16) bytes: `sync(1) ∥ core_seq(1) ∥ evt(u16 LE) ∥
ts_ms(u32 LE) ∥ arg0(u32 LE) ∥ arg1(u16 LE) ∥ crc16(u16 LE)`. `crc16` shall be
CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no final XOR) over bytes 0..13.
Records shall be encoded by **explicit byte offset**, never by overlaying a `struct`. *(D2.)*

### REQ-LOG-003 — text and records share one stream
`LOG_SYNC` shall be a value ≥ `0x80`, so ASCII runs and binary records coexist without escaping.
A decoder shall pass non-record byte runs through verbatim. `log_text()` shall reject bytes with
the high bit set. **No test shall assert on text output.** *(D3, D6.)*

### REQ-LOG-004 — one ring per image; the security core does not log
Each image shall own exactly **one** single-producer/single-consumer byte ring of power-of-two
capacity, in ordinary image-private RAM, written and drained by the same core.

The security core (CM0+) **shall not** produce log records. Per ADR-0021 D1 the SecOC verdict
is decided on the application core, so no contract event originates on the CM0+; a shared-RAM
channel out of the core holding the symmetric secret (REQ-SECOC-011) would be TCB surface
across the security boundary with no contract evidence in return. M0+ health shall be reported
from the application side instead, via the `ERROR` verdict (ADR-0016 D2/D5) and
`LOG_EVT_CRYPTO_ERROR`.

`LOG_CORE_SECURITY` shall remain defined in the wire format so revisiting this needs no
format change. *(D4; revised 2026-08-16 — see the ADR for the withdrawn per-core design.)*

### REQ-LOG-005 — the producer never blocks
`log_evt()` shall be callable from task or ISR context on any core, shall complete in bounded
time, and shall never block, spin, or wait on the sink. It shall return no status.

Its interrupt-masked critical section shall span **reservation, payload copy and head
publication as one indivisible step**, and shall never include the UART. The ring head shall
advance monotonically and shall never name bytes that have not been written — releasing the
mask between reserve and publish permits an ISR to publish over a task's unwritten
reservation, and permits head to move backwards. The masked duration shall be bounded by
`LOG_REC_SIZE` for records and `LOG_TEXT_MAX` for text. *(D7.)*

### REQ-LOG-006 — loss is bounded, counted, and announced
When a ring lacks room the producer shall drop the **newest** record (never advance the
consumer's tail) and increment a drop counter. When space frees it shall emit
`LOG_EVT_OVERFLOW` carrying the number lost. The `seq` field shall let a consumer
detect loss **independently** of that record, so a lost overflow report does not hide the loss.
*(D5.)*

### REQ-LOG-007 — a lossy window is a failed window
A consumer that asserts on the **absence** of an event shall first confirm no drops occurred in
that window (via `LOG_EVT_OVERFLOW` or a `seq` gap) and shall **fail** the window otherwise. A
suite reporting success from a window it knows was lossy is a false pass and is prohibited.
*(D5; this is the BVT's obligation, not the firmware's.)*

### REQ-LOG-008 — assertability is determined by ID range
Event IDs `< LOG_EVT_DIAG_BASE` (`0x1000`) are **contract** events: tests may assert on them,
and changing or removing one is a breaking change that shall update the affected tests in the
same commit. IDs `≥ 0x1000` are **diagnostic**: no test shall assert on them, so they remain
free to add, reword, or delete. *(D6.)*

### REQ-LOG-009 — SecOC verdicts are observable
The SecOC receive path shall emit a contract event for each verdict — accept, MAC failure,
freshness failure, resync — carrying the CAN ID and the reason. These events are the observable
form of the per-reason drop counters already required by **REQ-SECOC-001**; the two shall not
diverge. Emission shall occur on the verify path itself, after the verdict and before or with
the drop. *(D6; ADR-0021 D9.)*

### REQ-LOG-010 — the panic path
`log_panic()` shall bypass the ring and write its record with a blocking, polled, interrupt-free
UART write. It is the only logging function permitted to spin, and shall be called only from
fault/abort contexts where no drain task will run. *(D7.)*

### REQ-LOG-011 — host-testable core
`shared/log/src/log.c` shall compile and unit-test on the host with plain GCC and shall not
include vendor/ModusToolbox headers. All hardware — UART, clock, interrupt masking, ring
storage — shall sit behind `log_port_*`, with one link-time implementation per image and a fake
in `tests/`. *(D8; ADR-0001.)*

### REQ-LOG-012 — one registry, mechanically enforced
Event IDs, names, classes and argument meanings shall have exactly one source
(`shared/log/events.csv`), from which the C header and the host decoder table are generated. CI
shall fail if either generated artefact is stale. Hand-editing a generated file is prohibited.
*(D9.)*

### REQ-LOG-013 — encoder/decoder equivalence
A host test shall encode records with the target encoder and decode them with the host decoder,
asserting round-trip equality including CRC and mid-stream resync. A format drift between C and
Python shall fail this test rather than a bench run. *(D8.)*

### REQ-LOG-014 — bounded drain latency
The drain shall publish a **maximum latency** `LOG_DRAIN_LATENCY_MS` from `log_evt()` returning
to the record leaving the sink, under a defined worst-case queue depth. A consumer asserting the
**absence** of an event shall wait at least this long before concluding absence. The drain task
shall run at a period no greater than this bound and shall be schedulable at low priority
without violating it. *(D10.)*

## Notes on tagging

Per `docs/traceability-convention.md` §2, implementations carry `/* @impl REQ-LOG-nnn : … */`
and tests carry `/* @test REQ-LOG-nnn : … */`. `REQ-LOG-004` is partly `@design-only` — the
memory *placement* is a linker/MPU property provable only on target; the ring *algorithm* is
host-tested.
