# uds_seam4_probe

Bench tool to verify the FBL's real UDS session (M3 Seam 4): `0x10`
DiagnosticSessionControl, unknown-SID negative response, and `0x11` ECUReset — as
actual UDS semantics (request/response differ, or share only the SID), not an echo
check. Everything here fits a Single Frame; multi-frame segmentation is already
proven separately by `isotp_bringup_probe`.

**Not standard ISO-TP/UDS wire format.** Same simplified PCI layout as
`isotp_bringup_probe` (see `shared/diag/include/isotp_types.h`) and this project's
own simplified routineControl/download request layouts where relevant — a generic
UDS tool will not talk to this FBL.

## Setup (once)
Same as `can_echo_probe`/`isotp_bringup_probe` — Vector XL driver + `python-can`.

## Prerequisite on the FBL side
Build and flash the bootloader with Seam 4's composition root
(`fbl_diag.c`/`.h`) and get it resident in programming mode (LED blinking)
before running this.

## Run
```bash
python uds_seam4_probe.py                 # uses ./config.json
python uds_seam4_probe.py --config other.json
# -> "4/4 checks passed"
```

The last check (ECUReset) causes the FBL to reset immediately after sending its
positive response — the script cannot observe the reset itself, only the response
that precedes it. Confirm the reset happened physically (LED behaviour change) or
via the debugger.
