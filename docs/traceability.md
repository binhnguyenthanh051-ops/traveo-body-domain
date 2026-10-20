# Traceability report

> **Generated** by `host_tools/trace_report.py` — do not edit by hand. Regenerate with
> `python host_tools/trace_report.py`; `--check` fails if this file is stale.
> Tag scheme: `docs/traceability-convention.md`. Definition of Done: `docs/project/plan.md`
> rule 11.

## Summary

| Area | Source | REQs | Fully traced | With gaps |
|---|---|---|---|---|
| REQ-LOG | `docs/requirements/logging.md` | 14 | 6 | 8 |
| REQ-SECOC | `docs/requirements/secoc.md` | 13 | 0 | 13 |

**40 gap(s)** in total — see [Gaps](#gaps).

## REQ-LOG

| REQ | Requirement | Traces | Impl | Test | On silicon | Status |
|---|---|---|---|---|---|---|
| REQ-LOG-001 | Producer emits fixed-size binary records; no `stdio`/varargs/formatting on target | ADR-0023 D1; ADR-0003 | `shared/log/src/log.c` | — | *(no column)* | ❌ matrix-drift, no-test |
| REQ-LOG-002 | Record layout, LE fields, CRC-16/CCITT-FALSE, 16 B, byte-offset encoded | ADR-0023 D2 | `shared/log/src/log.c` ×2 | `shared/log/tests/test_log.c` ×3 | *(no column)* | ✅ |
| REQ-LOG-003 | `LOG_SYNC` outside ASCII; text runs pass through unescaped and are never assertable | ADR-0023 D3, D6 | `shared/log/src/log.c` | `shared/log/tests/test_log.c` ×5 | *(no column)* | ❌ matrix-drift |
| REQ-LOG-004 | One SPSC ring **per image**, image-private RAM; the security core does not log | ADR-0023 D4; ADR-0021 D1; ADR-0016 D2 | `shared/log/src/log.c` | `shared/log/tests/test_log.c` | *(no column)* | ✅ |
| REQ-LOG-005 | `log_evt()` is bounded, non-blocking, ISR-safe; critical section covers reservation only | ADR-0023 D7 | `shared/log/src/log.c` ×2 | `shared/log/tests/test_log.c` ×6 | *(no column)* | ✅ |
| REQ-LOG-006 | Overflow drops the **newest**, counts it, and emits `LOG_EVT_OVERFLOW`; `seq` gaps detect loss independently | ADR-0023 D5 | `shared/log/src/log.c` | `shared/log/tests/test_log.c` ×4 | *(no column)* | ✅ |
| REQ-LOG-007 | A consumer shall treat any overflow within a window as **failing** that window | ADR-0023 D5 | — | — | *(no column)* | ❌ matrix-drift, no-impl, no-test |
| REQ-LOG-008 | Contract IDs (`< 0x1000`) are assertable and breaking to change; diagnostic IDs are never asserted | ADR-0023 D6 | — | — | *(no column)* | ❌ matrix-drift, no-impl, no-test |
| REQ-LOG-009 | SecOC accept/reject events emitted with reason + CAN ID, on the verify path | ADR-0023 D6, D11; ADR-0021 D9, D5 | `node_a_gateway/app/src/health_task.c`<br>`node_a_gateway/bootloader/proj_cm4/src/fbl_main.c` ×2<br>`node_b_actuator/proj_cm7/src/main.c`<br>`shared/secoc/src/secoc_rx.c` | `shared/secoc/tests/test_secoc_rx.c` ×9 | *(no column)* | ✅ |
| REQ-LOG-010 | `log_panic()` bypasses the ring with a blocking, interrupt-free write | ADR-0023 D7 | `shared/log/src/log.c` | `shared/log/tests/test_log.c` ×2 | *(no column)* | ✅ |
| REQ-LOG-011 | Logic host-testable with plain GCC; all hardware behind `log_port_*` | ADR-0023 D8; ADR-0001 | `shared/log/src/log.c` | — | *(no column)* | ❌ matrix-drift, no-test |
| REQ-LOG-012 | One generated event registry; stale generated files fail CI | ADR-0023 D9 | — | — | *(no column)* | ❌ matrix-drift, no-impl, no-test |
| REQ-LOG-013 | C encoder and host decoder round-trip identically | ADR-0023 D8 | — | — | *(no column)* | ❌ matrix-drift, no-impl, no-test |
| REQ-LOG-014 | Drain latency is bounded and published, so absence-assertions can settle | ADR-0023 D10 | `shared/log/src/log.c` | `shared/log/tests/test_log.c` ×3 | *(no column)* | ❌ matrix-drift |

## REQ-SECOC

| REQ | Requirement | Traces | Impl | Test | On silicon | Status |
|---|---|---|---|---|---|---|
| REQ-SECOC-001 | Protected messages authenticated; unauthenticated dropped before decode | ADR-0021 D1, D9; ADR-0016; ADR-0023 D11 | `node_a_gateway/app/src/secoc_app.c`<br>`node_b_actuator/proj_cm7/src/secoc_app.c`<br>`shared/secoc/src/secoc.c`<br>`shared/secoc/src/secoc_rx.c` | `shared/secoc/tests/test_secoc.c` ×6<br>`shared/secoc/tests/test_secoc_rx.c` ×6 | ⏳ | ❌ no-silicon |
| REQ-SECOC-002 | MAC is AES-CMAC truncated to 8 B, constant-time compared | ADR-0021 D1 | `shared/secoc/src/secoc.c` | `shared/secoc/tests/test_secoc.c` | ⏳ | ❌ matrix-drift, no-silicon |
| REQ-SECOC-003 | Secured frame layout `PDU∥freshness(4)∥MAC(8)`, one CAN FD frame, LE wire | ADR-0021 D2, D3 | `shared/secoc/src/secoc.c` | `shared/secoc/tests/test_secoc.c` ×6 | ⏳ | ❌ no-silicon |
| REQ-SECOC-004 | MAC input binds a Data ID: `CMAC(DataID∥freshness∥PDU)` | ADR-0021 D2 | `shared/secoc/src/secoc.c` | `shared/secoc/tests/test_secoc.c` | ⏳ | ❌ no-silicon |
| REQ-SECOC-005 | Freshness = `epoch(u16)∥counter(u16)`; counter is per CAN-ID | ADR-0021 D3 | `shared/secoc/src/secoc_freshness.c` | `shared/secoc/tests/test_secoc_freshness.c` ×2 | ⏳ | ❌ no-silicon |
| REQ-SECOC-006 | Epoch shared per-node, persisted, `++`/boot; `commit(E+1)` durable **before** spend | ADR-0021 D3, D8 | `shared/secoc/src/secoc_freshness.c`<br>`shared/secoc_store/src/secoc_store.c` ×2 | `shared/secoc/tests/test_freshness_store.c` ×6<br>`shared/secoc/tests/test_secoc_freshness.c` ×2<br>`shared/secoc_store/tests/test_secoc_store.c` ×6 | ⏳ | ❌ no-silicon |
| REQ-SECOC-007 | Receiver accept rule (floor + per-ID high-water); MAC-check before freshness update | ADR-0021 D4 | `shared/secoc/src/secoc_freshness.c` ×2<br>`shared/secoc/src/secoc_rx.c` | `shared/secoc/tests/test_secoc_freshness.c` ×5<br>`shared/secoc/tests/test_secoc_rx.c` ×3 | ⏳ | ❌ matrix-drift, no-silicon |
| REQ-SECOC-008 | Receiver-reset resync via authenticated `FRESHNESS_SYNC` (0x2F0); `floor=persisted+1` | ADR-0021 D5; ADR-0002 | `node_a_gateway/app/src/secoc_app.c`<br>`node_b_actuator/proj_cm7/src/secoc_app.c`<br>`shared/secoc/src/secoc_freshness.c` ×2<br>`shared/secoc/src/secoc_rx.c` | `shared/secoc/tests/test_secoc_freshness.c`<br>`shared/secoc/tests/test_secoc_resync.c`<br>`shared/secoc/tests/test_secoc_rx.c` | ⏳ | ❌ no-silicon |
| REQ-SECOC-009 | Counter rollover bumps the epoch (durable) and resets counters | ADR-0021 D3 | `shared/secoc/src/secoc_freshness.c` | `shared/secoc/tests/test_secoc_freshness.c` | ⏳ | ❌ no-silicon |
| REQ-SECOC-010 | `key_id`-selected key; unknown `key_id` ⇒ fail (never key 0) | ADR-0021 D6; ADR-0019 D3 | `shared/crypto/src/crypto_keystore.c` | `shared/crypto/tests/test_crypto_mac.c` ×2<br>`shared/secoc/tests/test_secoc.c` | ⏳ | ❌ no-silicon |
| REQ-SECOC-011 | Secret key resides only in the M0+ image; app uses the MAC oracle only | ADR-0021 D6, D7; ADR-0017; ADR-0020 | `node_a_gateway/bootloader/proj_cm0p/src/crypto_ops_cm0p.c`<br>`node_b_actuator/proj_cm0p/src/crypto_ops_cm0p.c`<br>design-only: `node_a_gateway/bootloader/proj_cm0p/src/crypto_ops_cm0p.c`<br>design-only: `node_b_actuator/proj_cm0p/src/crypto_ops_cm0p.c` | `shared/crypto/tests/test_crypto_mac.c` | ⏳ | ❌ no-silicon |
| REQ-SECOC-012 | `CRYPTO_OP_MAC=0x03` generic op: `key_id∥len∥msg ⇒ tag[16]` | ADR-0021 D7; ADR-0017 D3 | `shared/crypto/src/crypto_msg.c` ×4 | `shared/crypto/tests/test_crypto_mac.c` ×3 | ⏳ | ❌ no-silicon |
| REQ-SECOC-013 | Telemetry `0x200` (B→A) is authenticated on the same path | ADR-0021 D7 | `node_b_actuator/proj_cm7/src/secoc_app.c`<br>`shared/secoc/src/secoc.c` | `shared/secoc/tests/test_secoc.c` | ⏳ | ❌ no-silicon |

## Gaps

- **REQ-LOG** · `no-silicon-col` · requirements file has no "On silicon" column — `docs/requirements/logging.md`
- **REQ-LOG-001** · `matrix-drift` · matrix names `test_log`, but it has no test tag for REQ-LOG-001 — `docs/requirements/logging.md`
- **REQ-LOG-001** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-003** · `matrix-drift` · matrix names `test_log_decode`, but it has no test tag for REQ-LOG-003 — `docs/requirements/logging.md`
- **REQ-LOG-007** · `matrix-drift` · matrix names `test_bvt_harness`, but no such test file exists — `docs/requirements/logging.md`
- **REQ-LOG-007** · `no-impl` · no impl tag (and no design-only tag)
- **REQ-LOG-007** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-008** · `matrix-drift` · matrix names `test_log_events`, but no such test file exists — `docs/requirements/logging.md`
- **REQ-LOG-008** · `no-impl` · no impl tag (and no design-only tag)
- **REQ-LOG-008** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-011** · `matrix-drift` · matrix names `test_log`, but it has no test tag for REQ-LOG-011 — `docs/requirements/logging.md`
- **REQ-LOG-011** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-012** · `matrix-drift` · matrix names `test_log_events`, but no such test file exists — `docs/requirements/logging.md`
- **REQ-LOG-012** · `no-impl` · no impl tag (and no design-only tag)
- **REQ-LOG-012** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-013** · `matrix-drift` · matrix names `test_log_decode`, but it has no test tag for REQ-LOG-013 — `docs/requirements/logging.md`
- **REQ-LOG-013** · `no-impl` · no impl tag (and no design-only tag)
- **REQ-LOG-013** · `no-test` · no test tag (and no design-only tag)
- **REQ-LOG-014** · `matrix-drift` · matrix names `test_bvt_harness`, but no such test file exists — `docs/requirements/logging.md`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/src/log.c:342`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/tests/test_log.c:309`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/tests/test_log.c:320`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/tests/test_log.c:338`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/tests/test_log.c:349`
- **REQ-LOG-015** · `dangling` · tag names an undefined requirement — `shared/log/tests/test_log.c:355`
- **REQ-SECOC-001** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-002** · `matrix-drift` · matrix names `test_crypto_mac`, but it has no test tag for REQ-SECOC-002 — `docs/requirements/secoc.md`
- **REQ-SECOC-002** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-003** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-004** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-005** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-006** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-007** · `matrix-drift` · matrix names `test_secoc`, but it has no test tag for REQ-SECOC-007 — `docs/requirements/secoc.md`
- **REQ-SECOC-007** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-008** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-009** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-010** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-011** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-012** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`
- **REQ-SECOC-013** · `no-silicon` · no silicon evidence yet — `docs/requirements/secoc.md`

## Deferred on silicon

None.

## ADR coverage (informational, not gated)

Accepted ADRs without a test tag are listed, not failed: M1–M4 tags are backfilled
opportunistically (`docs/traceability-convention.md` §6).

| ADR | Title | Status | Impl tags | Test tags | Note |
|---|---|---|---|---|---|
| ADR-0001 | Keep hardware-independent logic host-testable | accepted | 0 | 0 | no test tag |
| ADR-0002 | CAN ID allocation scheme | accepted | 0 | 0 | no test tag |
| ADR-0003 | Adopt MISRA C:2012 as the coding standard | accepted | 0 | 0 | no test tag |
| ADR-0004 | Bootloader + application split in one mono-repo | accepted | 0 | 0 | no test tag |
| ADR-0005 | FreeRTOS for the application; custom scheduler as a standalone deep-dive | accepted | 0 | 0 | no test tag |
| ADR-0006 | Security architecture — vetted primitives, two-layer secure boot | accepted | 0 | 0 | no test tag |
| ADR-0007 | Boot-state persistence — `.noinit` shared region + backup registers, reset-cause classification | accepted | 0 | 0 | no test tag |
| ADR-0008 | Boot-decision policy, knock window, and the app jump | accepted | 0 | 0 | no test tag |
| ADR-0009 | Scheduler design — fixed-priority preemptive with portable core | accepted | 0 | 0 | no test tag |
| ADR-0010 | FreeRTOS integration for the Node A application | accepted | 0 | 0 | no test tag |
| ADR-0011 | CANFD channel configuration for the Node A application | accepted | 0 | 0 | no test tag |
| ADR-0012 | UDS diagnostic-stack layering (M3) | accepted | 0 | 0 | no test tag |
| ADR-0013 | ISO-TP transport configuration (M3) | accepted | 0 | 0 | no test tag |
| ADR-0014 | UDS session and security-access design (M3) | accepted | 0 | 0 | no test tag |
| ADR-0015 | Resource & lifecycle manager (`sysmgr`) and the fail-safe catalog | accepted | 0 | 0 | no test tag |
| ADR-0016 | App signature verification — the FBL→app authenticity layer (M4) | accepted | 0 | 0 | no test tag |
| ADR-0017 | M4↔M0+ crypto-service architecture — the offload boundary (M4) | accepted | 0 | 0 | no test tag |
| ADR-0018 | Cross-core IPC transport — shared-RAM mailbox + hardware semaphore (M4) | accepted | 0 | 0 | no test tag |
| ADR-0019 | Key handling, algorithm, and the signing tool (M4) | accepted | 0 | 0 | no test tag |
| ADR-0020 | TCB isolation — enforcing the CM0+ crypto boundary with protection units (M4 Seam 5) | accepted | 0 | 0 | no test tag |
| ADR-0021 | SecOC protocol — framing, freshness, resync, and symmetric-key handling (M5) | accepted | 17 | 11 |  |
| ADR-0023 | Target logging channel — structured events over UART, assertable by the BVT | proposed | 3 | 1 |  |
| ADR-0024 | Persistent freshness store — a two-slot record in work flash (M5B) | proposed | 4 | 10 |  |
