# Coding standard

## Standard: MISRA C:2012 (with Amendment 1/2 guidance)

Target/production C in this project is written to **MISRA C:2012**. This is the
embedded-automotive default and is the discipline the portfolio is meant to demonstrate.

> Honesty note: this is a *learning/portfolio* project. "Written to MISRA" here means the
> rules are the standard we code to and check automatically where we can — not that the
> project carries a formal, audited compliance certificate. Deviations are documented
> rather than hidden (that documentation *is* the MISRA-compliant way to deviate).

## Scope

| Code | MISRA applies? |
|------|----------------|
| Target firmware + hardware-independent logic (`scheduler`, `eeprom_emu`, `common/messages`, `security`, `common/hal`, node apps) | **Yes** |
| Host test harnesses (`*/tests/`) | No — test code may use `printf`, function-like macros, etc. Treated as off-target tooling. |
| Python host tools (`host_tools/`) | N/A (not C) |

Keeping production logic MISRA-clean while letting test harnesses use `printf`/macros is a
standard and defensible split — it's worth a sentence in the EP.03/EP.09 posts.

## Enforcement

- Automatable rules are checked with **cppcheck**'s MISRA addon in CI (`make lint`).
  cppcheck is free; the MISRA addon needs a rule-texts file you supply locally (the rule
  *texts* are copyrighted by MISRA, the addon is not). Until that file is in place, CI runs
  cppcheck's general static analysis, which already catches a large overlapping set.
- Not every MISRA rule is statically decidable; the rest are upheld by review and by the
  rules in `CLAUDE.md` so Claude Code applies them while generating code.

## Naming conventions

Formalises the pattern already used by `shared/boot`, `shared/can`, `shared/hal`, and
`shared/messages` — new modules (M3's diagnostic stack, `sysmgr`, etc.) follow this rather
than inventing per-module style.

### Two seam shapes — pick one per interface, don't invent a third

| Shape | Form | Existing examples | Pick this when... |
|---|---|---|---|
| **HAL interface struct** | `struct` of function pointers only, `_if_t` suffix, possibly multiple instances | `can_hal_if_t`, `hal_flash_if_t`, `hal_crypto_if_t` | the thing is swappable or multi-instance (a Strategy, a resource with more than one implementation) |
| **Port singleton free-functions** | `<owner>_port_<verb>()`, one link-time implementation (target `.c` vs. test fake `.c`), no struct | `fbl_port_*`, `app_port_*` | there is exactly one of it per image (reset, tick, tool-contact) |

A config/descriptor struct that mixes **data fields with embedded callbacks** (e.g.
`sysmgr_resource_t`: dependency list, timeout, retry policy, *plus* `init`/`poll_ready`/
`deinit` pointers) is **neither** — it stays a plain `_t`, not `_if_t`. `_if_t` is reserved
for structs that are *only* function pointers (a pure vtable).

### Files

- Public header other modules `#include`: `<module>.h` in `include/`.
- Pure types shared by multiple consumers, no functions: `<module>_types.h` (mirrors
  `boot_types.h`).
- Internal-only code: no separate `_priv.h` — helpers stay `static` inside the `.c` file.
  Only add a `<module>_internal.h` (kept in `src/`, never `include/`) if a module genuinely
  spans multiple `.c` files that must share private helpers; don't add it speculatively.
- Tests: `test_<module>.c`, one runner per module.
- Port/HAL fakes: one bundled `<owner>_port_fake.c` per image (matches `boot_port_fake.c`,
  `app_port_fake.c`), not one fake file per interface.

### Functions

Grammar: **`<module>[_<submodule>]_<verb>[_<object>]`** — `<module>` is the header the
function is declared in, not the SID/hex value it happens to implement (`uds_download_start`,
not `uds_h34_start`; hex codes stay in `UDS_SID_*` macros/comments).

Port-crossing calls always contain the literal word `port`: `fbl_port_tool_contact`,
`fbl_port_system_reset`.

Internal (`static`) helpers keep the **same module prefix** as the public API in that file
(readable stack traces / map files) — just `static` and never declared in a header. Never a
leading underscore: MISRA Rule 21.1/21.2 reserve those identifiers.

No version markers in names (no `<module>_v2_foo`) — internal APIs here aren't versioned.

### Variables

- Plain snake_case nouns, no Hungarian/type prefix (`magic`, `image_len`, `msp`) — the
  fixed-width typedef already states the type. No `p_`/`pp_` on pointers either.
- `g_` prefix for module-scope singleton state (precedent: `g_handshake`, ADR-0007 D8) —
  e.g. `g_uds_session`, `g_download_state`.
- Constants/macros: `UPPER_SNAKE`, module-prefixed (`FBL_BOOTLOOP_THRESHOLD`,
  `UDS_SID_ROUTINE_CONTROL`, `ISOTP_MAX_PAYLOAD`).

### Types

- `_t` suffix on every typedef, no exceptions.
- `_if_t` suffix reserved for pure function-pointer interface structs (see the seam-shape
  table above).
- Enums: `<MODULE>_<SHORT-CATEGORY>_<VALUE>` (`FBL_RST_POWER_ON`, `UDS_SESS_PROGRAMMING`,
  `UDS_SEC_UNLOCKED`).

## Accepted deviations (living list)

Record each deviation here with rule, location, and rationale. Starting set:

| Rule | Where | Rationale |
|------|-------|-----------|
| 15.5 (single point of exit — *advisory*) | guard-clause functions in `common/messages`, etc. | Early-return on invalid input is clearer and safer than a single-exit flag dance. Advisory rule, deliberately deviated. |
| 21.6 (no `<stdio.h>`) | `*/tests/` only | Host test harnesses use `printf`. Production code never includes `<stdio.h>`. |
| Dir 1.1 / Rule 1.2 (no reliance on unspecified/undefined behaviour; no language extensions) | `scheduler/src/sched.c` — `__builtin_ctz` in `sched_find_highest_ready_priority` | The priority-bitmap selector uses the `__builtin_ctz` compiler intrinsic (count-trailing-zeros → `RBIT+CLZ` on Cortex-M, `BSF/TZCNT` on x86). No portable C standard equivalent exists with the same single-instruction codegen. The argument is guarded against 0 at the call site (`ctz(0)` is undefined), and a portable binary-search fallback is documented in ADR-0009 §D3 if the extension must be removed. Encapsulated in one function. |
| Rule 4.3 (assembly encapsulated in dedicated functions) | `scheduler/src/sched_port_arm.c` (target port: PendSV/SysTick context switch) | The context switch requires inline/naked assembly (PSP manipulation, `{r4–r11}` + FP save/restore, EXC_RETURN). It is confined to the dedicated target-port translation unit, behind the `sched_port.h` interface; the host fake contains no assembly and the portable core never sees it. |
| Rule 4.3 (assembly encapsulated in dedicated functions) | `node_a_gateway/bootloader/src/port_jump.c` — `fbl_port_deinit_for_jump` / `fbl_port_jump_to_app` | The FBL→app handover needs the `cpsid i` de-init and the naked VTOR/MSP/branch helper (nothing stack-dependent between the MSP-set and the branch — ADR-0008 D4). Confined to the target jump port; the host fake records the call instead. |
| Rule 11.4 (no conversion between integer and pointer) | `node_a_gateway/bootloader/src/{port_image.c, port_noinit.c}` and `port_jump.c` asm | Addressing memory-mapped flash, the `.noinit` region, and `SCB->VTOR` requires integer↔pointer conversion. Confined to the target port accessors; the portable core in `shared/boot` never does this. |

Add rows as real deviations come up — an honest, growing deviation log is itself a strong
architect signal.
