# uds_seam5_probe

Bench tool to verify the FBL's real SecurityAccess round trip (M3 Seam 5): `0x27`
requestSeed/sendKey, correct-key unlock, wrong-key rejection + relock, and the
sequence-error cases (sendKey without a fresh seed, sendKey after a failed
attempt).

The seed/key transform (`seed ^ 0xA5A5A5A5`) is deliberately simple and **not a
secret** — see the honesty note in `shared/diag/include/uds_security_access.h`:
this demonstrates the state-machine mechanism, not a real security boundary. A
real PC tool would need to know this transform the same way this script does.

**Not standard ISO-TP/UDS wire format** — same simplified PCI layout as the other
probes in `host_tools/`.

## Setup (once)
Same as the other probes — Vector XL driver + `python-can`.

## Prerequisite on the FBL side
Build and flash the bootloader with Seam 5's dispatch-table addition
(`uds_security_access_handler()` wired into `fbl_diag.c`) and get it resident in
programming mode before running this.

## Run
```bash
python uds_seam5_probe.py                 # uses ./config.json
python uds_seam5_probe.py --config other.json
# -> "6/6 checks passed"
```
