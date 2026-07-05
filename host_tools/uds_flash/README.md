# uds_flash

The PC-side flash tool for M3 (Seam 7): reflashes the Node A app over CAN by
driving the FBL's full UDS programming sequence.

Steps it performs (all over this project's simplified ISO-TP / UDS — **not**
standard ISO 15765-2/14229, so a generic UDS tester won't interoperate):

1. `0x10 02` — enter programming session
2. `0x27` — SecurityAccess seed/key (`key = seed ^ 0xA5A5A5A5`; see the honesty
   note in `shared/diag/include/uds_security_access.h` — mechanism, not real security)
3. `0x31 FF00` — routineControl **erase** (whole 32 KB sectors covering the image)
4. `0x34` — requestDownload (app base + image size)
5. `0x36` — transferData, block by block (row-aligned, honouring the server's
   maxNumberOfBlockLength)
6. `0x37` — requestTransferExit
7. `0x31 FF01` — routineControl **check image** (CRC32 verify)
8. `0x11 01` — ECUReset → the FBL's boot decision re-verifies and jumps to the app

The image is stamped with the FBL header + CRC32 trailer (ADR-0008 D3) via
`host_tools/fbl_image_stamp.py`, then padded up to the 512-byte flash program
row so every transferData block is row-aligned. The app's linker already
reserves the 12-byte header slot at `app_base + 0x100`, so stamping does not
clobber code.

## Setup (once)
Same as the other probes — Vector XL driver + `python-can`, channel mapped to
the `app_name` in Vector Hardware Config.

## Prerequisite on the FBL side
Build and flash the bootloader with the Seam 7 wiring (the download +
routineControl handlers in `fbl_diag.c`) and get it resident in programming
mode before running this.

## Run
```bash
python uds_flash.py ../../node_a_gateway/app/build/CYTVII-B-E-1M-SK/Debug/gateway_app.hex
python uds_flash.py app.hex --config other.json
```

On success the FBL resets and jumps to the freshly-flashed app — confirm by the
app's LED behaviour. The tool only sees up to the ECUReset request; it cannot
observe the jump itself.

## Config — `config.json`
- `uds.request_id` / `response_id` — `0x7A0` / `0x7A8` (ADR-0002 M3 band)
- `image.app_base` — `0x10040000` (FBL_APP_FLASH_BASE)
- `image.header_offset` — `0x100` (must equal FBL_APP_HEADER_OFFSET)
- `image.large_sector_size` — `0x8000` (32 KB, for rounding the erase length)
