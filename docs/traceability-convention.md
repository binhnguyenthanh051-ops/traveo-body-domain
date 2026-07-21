# Traceability & tagging convention

The keystone for the `checks/` pipeline. It gives deterministic scripts (and, later, the
advisory LLM reviewer) structure to work against: decisions, code, and tests all carry IDs
that link back to each other. Cheap to adopt, and it turns most "checks" into `grep`, not AI.

> Goal: every architectural decision is traceable to the code that implements it and the tests
> that prove it — mechanically, so a script (not my memory) catches a gap. Judgment stays mine;
> the tags just let a script tell me when I *forgot* to exercise it.

## 1. ID scheme

| Artifact | ID format | Where defined |
|---|---|---|
| Architecture Decision Record | `ADR-NNNN` (+ optional sub-decision `Dn`) | `docs/architecture/decisions/ADR-NNNN-*.md` |
| Review finding | `<series><n>` — e.g. `F1` (scheduler), `B1` (boot), `M2-3` | `docs/reviews/*.md` |
| Requirement (introduce for traceability) | `REQ-<area>-NNN` — e.g. `REQ-BOOT-012`, `REQ-SECOC-004` | `docs/requirements/*.md` |

IDs are stable and never reused. A superseded ADR stays on disk (status `superseded`), so its ID
never dangles.

## 2. Reference tags in code and tests

A one-line comment tag links an implementation or a test back to the decision/requirement it
serves. Format — a fixed keyword so it's greppable:

```c
/* @impl ADR-0009 D5  : PendSV context switch, FP lazy-stacking */
/* @impl REQ-BOOT-012 : app-invalid => stay in FBL (fail-safe) */
```
```c
/* @test ADR-0009 D5  : saves/restores s16-s31 only when EXC_RETURN bit4=0 */
/* @test REQ-BOOT-012 : invalid digest => decision == STAY_IN_FBL */
```

Rules:
- `@impl <ID>` marks code that *implements* a decision/requirement.
- `@test <ID>` marks a test that *verifies* one.
- One tag per line; multiple tags = multiple lines. A file/function may carry several.
- The `<ID>` token matches `ADR-\d{4}( D\d+)?`, `REQ-[A-Z]+-\d{3}`, or a review-finding ID.
- Keep the prose after the `:` short — it's a human hint, not parsed.

## 3. What this makes deterministically checkable (no AI)

- **ADR ↔ test:** every ADR with status `accepted` has ≥1 `@test ADR-XXXX` somewhere. A gap = a
  decision with no proof. (Sub-decisions `Dn` can be required individually if you want it strict.)
- **ADR ↔ impl:** every accepted ADR has ≥1 `@impl` (or is explicitly design-only, tagged
  `@design-only`).
- **Requirement coverage:** every `REQ-*` has an `@impl` and a `@test`.
- **No dangling tags:** every `@impl`/`@test` ID refers to an ID that actually exists on disk.
- **Traceability matrix:** parse all tags → emit `REQ/ADR → impl files → test files` as a table
  (`docs/traceability.md`, generated, git-ignored or committed as an artifact).

## 4. Layer / architecture invariants (separate from tags, also deterministic)

Encoded as forbidden-dependency rules, checked by `grep`-style scripts:

- **Host-testability (ADR-0001):** no vendor/ModusToolbox headers in host-testable modules.
  Rule: files under `shared/**` (except `**/port*`/target dirs) must not `#include` `cy_*` /
  vendor SDK headers, and must not call FreeRTOS APIs.
- **Layer isolation (diagnostic stack / bridge):** the protocol/message layer must not reference
  transport/CAN/flash symbols directly; services must not reference queue/SPI handles. Each layer
  declares its **forbidden symbols/includes**; the script greps for violations.
- Rules live in a small machine-readable file (below) so they're data, not hard-coded.

## 5. Rule data file (so checks are config, not code)

`checks/rules.yml` (or `.json`) — example shape:
```yaml
host_testable:
  paths: ["shared/**"]
  exclude: ["shared/**/port*", "shared/**/*_target*"]
  forbid_includes: ["cy_*", "cyhal_*", "cybsp_*"]
  forbid_symbols: ["xTaskCreate", "xQueueSend", "vTaskDelay"]
layers:
  - name: uds_protocol
    paths: ["shared/diag/uds/**"]
    forbid_symbols: ["Cy_CAN*", "*_flash_write", "xQueue*"]  # protocol must not touch transport/flash
  - name: iso_tp_transport
    paths: ["shared/diag/isotp/**"]
    forbid_symbols: ["uds_dispatch", "uds_session_*"]        # transport must not know services
```
Adding a layer = adding a data entry, not editing the checker. (Same "config over code" instinct
as the FBL/app variants.)

## 6. Adoption (low-friction, incremental)

- Apply tags going forward from M5; backfill M1–M4 opportunistically (a good rainy-day task,
  and it doubles as a re-read of your own code).
- The checks run in CI alongside lint/tests and gate merges (branch protection already enforces).
- Keep it boring and deterministic — anything needing judgment is NOT in this layer; it goes to
  the advisory LLM reviewer (separate, non-gating, built later only if manual review is the
  bottleneck).
