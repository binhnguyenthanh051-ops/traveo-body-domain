# isotp_bringup_probe

Bench tool to verify the FBL's ISO-TP transport on the real bus (M3 Seam 2). Sends a
message to the diagnostic request ID and confirms the FBL's bring-up echo
(`run_isotp_bringup_echo`, `node_a_gateway/bootloader/src/port_prog.c`) comes back
correctly on the response ID -- proving reassembly and segmentation both directions.

**Not standard ISO-TP.** This project's transport uses a deliberately simplified PCI
layout (see `shared/diag/include/isotp_types.h`), not byte-exact ISO 15765-2. Standard
tools/libraries (`python-can-isotp`, CANoe, etc.) will not talk to the FBL -- this script
implements the matching simplified framing directly.

## Setup (once)
Same as `can_echo_probe` — Vector XL driver + `python-can`, channel assigned to the
`app_name` in Vector Hardware Config.

## Configure — `config.json`
- `test.request_id` / `test.response_id` — Node A diagnostic IDs (ADR-0002 M3 extension:
  `0x7A0` request, `0x7A8` response).
- `test.messages` — a list of ASCII strings to round-trip. Keep at least one short (fits
  a Single Frame, <=62 bytes) and one long (forces First/Consecutive Frame + flow control)
  to exercise both paths.

## Prerequisite on the FBL side
Build and flash the bootloader with Seam 2's bring-up wiring
(`isotp_init`/`run_isotp_bringup_echo` in `port_prog.c`) and get it resident in
programming mode (LED blinking) before running this — the echo only runs inside
`fbl_port_enter_programming_mode()`'s loop, same as Seam 1.

## Run
```bash
python isotp_bringup_probe.py                 # uses ./config.json
python isotp_bringup_probe.py --config other.json
# -> "2/2 messages echoed correctly"
```
