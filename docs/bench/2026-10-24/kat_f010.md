# K — F-010 on silicon: CMAC known-answer test with fault injection, 2026-10-24

M5B bench, checklist block "K" (`docs/project/weeks/2026-W43.md`). Evidence for **F-010** (G4) and
**REQ-SECOC-010/012** on target: a crypto failure now reaches the caller instead of the CMAC
reporting success.

Images built with `CRYPTO_BRINGUP_KAT=1` from `m5b/secoc_improve` at `c2b3a5d` (F-010 fix `1f40dfc`).
Values were read from RAM with the debugger after boot. Pass = **`g_kat_result == 15`** (it was 7 on 2026-09-28,
before the fault check existed).

Flashed with the wrapper scripts:
- Node A: `zz_build_gateway_fbl.sh program cm0p CRYPTO_BRINGUP_KAT=1` (CM0+ region only)
- Node B: `zz_build_actuator_app.sh program all CRYPTO_BRINGUP_KAT=1`

## Result

| Node | Core | `g_kat_result` addr | Value | Expected |
|---|---|---|---|---|
| A — CYT2B7 | CM0+ (FBL image) | 0x08000F54 | 0x0F | 15 ✓ |
| B — CYT4BF | CM0+ | 0x28001280 | 0x0F | 15 ✓ |

The addresses match the 2026-09-28 run (`stage2_kat.md`).

## What the bits mean (`cm0p_cmac_kat`, `crypto_ops_cm0p.c`)

| Bit | Name | Meaning |
|---|---|---|
| 0x1 | `KAT_PASS_EMPTY` | empty-message CMAC = NIST SP 800-38B D.1 example 1 |
| 0x2 | `KAT_PASS_16B` | 16-byte CMAC = D.1 example 2 |
| 0x4 | `KAT_SHARED_OK` | CMAC with the SecOC shared secret was computed (A == B is a separate check by hand, below) |
| 0x8 | `KAT_FAULT_OK` | **F-010:** the Crypto bus-error flag is raised with `Cy_Crypto_Core_SetInterrupt(…BUS_ERROR…)`. The next CMAC **fails**, and the one after **passes with the correct tag**, so the flag is cleared and not sticky. |

Bit 3 being set on both nodes shows two things: an injected hardware fault reaches the caller as a failure,
which the SecOC ERROR verdict path depends on, and the core recovers on the next call.

## Not re-read this run

The tag arrays (`g_kat_tag_empty`, `g_kat_tag_16b`, `g_kat_shared_tag`) weren't dumped. Bits 0 and 1 already
compare the first two against the NIST vectors in firmware. The shared-secret tag's A == B equality was proven on
2026-09-28 (`6ca65e17…` on both nodes). The secret hasn't changed since, and the G2/G3 run the same evening had
thousands of cross-node MAC accepts (`g2_g3_reset_replay.md`), which is a stronger check of the same thing.

## For the "On silicon" column (Sun 25)

- **F-010** → Resolved. Evidence: this file (bit 3 on both CM0+).
- **REQ-SECOC-010 / 012** → this file. A crypto failure is reported, not silently treated as success.
