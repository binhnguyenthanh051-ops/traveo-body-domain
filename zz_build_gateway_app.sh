#!/usr/bin/env bash
#
# Build the Node A gateway APPLICATION (CM4 image) and produce a flashable,
# FBL-verifiable image at the app base (0x1004_0000):
#
#   1. make build            -> app .elf (CM0+ prebuilt @ 0x1000_0000 + CM4 @ 0x1004_0000)
#   2. objcopy strips the CM0+ prebuilt and emits the CM4 image as a raw .bin
#      (the FBL already provides CM0+; the app is CM4-only on the bus).
#   3. fbl_image_stamp.py writes the FBL header + CRC32 trailer.
#   4. objcopy re-addresses the stamped image to 0x1004_0000 as Intel HEX.
#
# Flash build/app_stamped.hex (the FBL then verifies + jumps to it). The FBL
# image is flashed separately by zz_build_gateway_fbl.sh.
#
# Run from a ModusToolbox shell (GCC_ARM toolchain + objcopy on PATH).

set -euo pipefail

APP_BASE=0x10040000          # FBL_APP_FLASH_BASE (boot_types.h)
HEADER_OFFSET=0x100          # FBL_APP_HEADER_OFFSET
OBJCOPY=arm-none-eabi-objcopy

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/node_a_gateway/app"

# Pass CY_TOOLS_DIR directly so $(wildcard C:/...) in the MTB Makefile resolves
# correctly under Git Bash's make, which does not expand C:/ wildcard paths.
if [ -n "${CY_TOOLS_PATHS:-}" ]; then
    make CY_TOOLS_DIR="$CY_TOOLS_PATHS" "${1:-build}"
else
    make "${1:-build}"
fi

ELF="$(ls build/*/Debug/*.elf 2>/dev/null | head -1)"
[ -n "$ELF" ] || { echo "no .elf in build/*/Debug/ — build first" >&2; exit 1; }

# CM4-only raw image: drop the CM0+ prebuilt and the high signature slot so the
# binary spans only the actual CM4 code/data (not the whole region). Then stamp,
# then re-address to HEX.
"$OBJCOPY" --remove-section=.cy_m0p_image --remove-section=.cy_app_signature \
    -O binary "$ELF" build/app_cm4.bin
# Windows Python can't read Cygwin-style paths; convert if cygpath is available.
_stamp_script="$(cygpath -w "$ROOT/host_tools/fbl_image_stamp.py" 2>/dev/null || echo "$ROOT/host_tools/fbl_image_stamp.py")"
python "$_stamp_script" \
    build/app_cm4.bin build/app_stamped.bin --header-offset "$HEADER_OFFSET"
unset _stamp_script
"$OBJCOPY" -I binary -O ihex --change-addresses "$APP_BASE" \
    build/app_stamped.bin build/app_stamped.hex

echo ""
echo "==> build/app_stamped.hex  (addressed at $APP_BASE — flash this; the FBL verifies + jumps)"
