# Key residency — the SecOC secret is only in the CM0+ images, 2026-10-30

Evidence for **REQ-SECOC-011** (M5-R8): the AES-CMAC shared secret lives only in each node's
CM0+ image, and no application-core image holds its bytes. Where the key sits is linker
composition (`@design-only` in the code), so the evidence is the built images, not a bench run.

**Tool:** `python host_tools/check_key_residency.py`. It reads the key from
`security/include/secoc_shared_secret.h` (the single source) and searches each ELF for the
16 bytes. It reports offsets only, never the bytes. Tests: `host_tools/tests/test_check_key_residency.py`.

**Images:** the Debug ELFs of the 2026-10-24 M5B bench builds (`m5b/secoc_improve` at `c2b3a5d`,
see `docs/bench/2026-10-24/g2_g3_reset_replay.md`). The app images are from 21:55–21:57; the two
CM0+ images were rebuilt at 22:36–22:37 as normal images after the KAT run, from the same tree.

## Result

| Role | Image | Expected | Seen | Verdict |
|---|---|---|---|---|
| CM0+ (Node A) | `node_a_gateway/bootloader/proj_cm0p/…/gateway_fbl_cm0p.elf` | key present | at file offset 0x7A10 | ✅ |
| CM4 FBL | `node_a_gateway/bootloader/proj_cm4/…/gateway_fbl.elf` | absent | absent | ✅ |
| CM4 app | `node_a_gateway/app/…/gateway_app.elf` | absent | absent | ✅ |
| CM0+ (Node B) | `node_b_actuator/proj_cm0p/…/actuator_cm0p.elf` | key present | at file offset 0x60FC | ✅ |
| CM7 app | `node_b_actuator/proj_cm7/…/actuator_cm7.elf` | absent | absent | ✅ |

The two CM0+ rows are the positive control: if the search could not find the key where it is
meant to be, an "absent" on the app images would prove nothing.

**Limits, stated plainly:** this shows the *bytes* are not linked into an app image. It does not
show that the CM0+'s memory is protected from the app core at run time. That is the SMPU/PPU
configuration (ADR-0020), which is a separate claim.
