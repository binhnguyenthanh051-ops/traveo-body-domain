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
#   5. (program only) flash build/app_stamped.hex over KitProg3 + reset (F-013).
#
# Usage:
#   ./zz_build_gateway_app.sh                    # steps 1-4
#   ./zz_build_gateway_app.sh program            # steps 1-5
#   ./zz_build_gateway_app.sh build VAR=value    # anything after the action goes to make
#   FBL_SIGN_KEY=other.pem ./zz_build_gateway_app.sh program   # override bench.env
#
# `program` never flashes MTB's raw gateway_app.hex: the FBL rejects it (no
# header, no stamp/signature). It flashes the image from step 3 with
# `make qprogram PROG_FILE=...`, so it reuses the recipe's OpenOCD config and
# its `program ... verify reset`. The FBL image is flashed separately by
# zz_build_gateway_fbl.sh.
#
# Two kits plugged in (F-012): set NODE_A_PROBE (in bench.env) to Node A's KitProg3 serial and
# the flash goes to that probe only (MTB_PROBE_SERIAL -> `adapter serial`).
# List the serials with
#   /c/Infineon/Tools/ModusToolboxProgtools-1.8/fw-loader/bin/fw-loader.exe --device-list
# Unset = OpenOCD takes the first probe it finds, so with two kits it may flash
# the wrong board.
#
# Run from a ModusToolbox shell (GCC_ARM toolchain + objcopy on PATH).

set -euo pipefail

APP_BASE=0x10040000          # FBL_APP_FLASH_BASE (boot_types.h)
HEADER_OFFSET=0x100          # FBL_APP_HEADER_OFFSET
OBJCOPY=arm-none-eabi-objcopy

ROOT="$(cd "$(dirname "$0")" && pwd)"

# Local bench settings (probe serials, signing key): bench.env at the repo root,
# gitignored; template bench.env.example. Values already in the environment win.
if [ -f "$ROOT/bench.env" ]; then
    . "$ROOT/bench.env"
fi

# Resolve a relative FBL_SIGN_KEY against the repo root BEFORE cd-ing into the
# app dir (a bare "ec_p256_dev_private.pem" would otherwise resolve against
# node_a_gateway/app and not be found).
if [ -n "${FBL_SIGN_KEY:-}" ]; then
    case "$FBL_SIGN_KEY" in
        /* | [A-Za-z]:*) : ;;                     # already absolute
        *) FBL_SIGN_KEY="$ROOT/$FBL_SIGN_KEY" ;;  # relative -> repo root
    esac
    if [ ! -f "$FBL_SIGN_KEY" ]; then
        echo "FBL_SIGN_KEY not found: $FBL_SIGN_KEY" >&2
        echo "  (generate it: python host_tools/gen_dev_key.py --out-priv ec_p256_dev_private.pem \\" >&2
        echo "     --out-header node_a_gateway/bootloader/proj_cm0p/src/crypto_pubkey_dev.h --key-id 1)" >&2
        exit 1
    fi
fi

cd "$ROOT/node_a_gateway/app"

# F-013: `make program` would flash MTB's raw gateway_app.hex BEFORE the stamp
# step, and the FBL rejects that image. So `make` only ever builds here; the
# flash, when asked for, is step 5 below, with the stamped/signed hex.
ACTION="${1:-build}"
EXTRA=("${@:2}")   # extra make args, as in the other zz_build_* scripts
case "$ACTION" in
    build|program) : ;;
    *) echo "Unknown action '$ACTION' -- use 'build' or 'program'" >&2; exit 1 ;;
esac

# Pass CY_TOOLS_DIR directly so $(wildcard C:/...) in the MTB Makefile resolves
# correctly under Git Bash's make, which does not expand C:/ wildcard paths.
MAKE_ARGS=("${EXTRA[@]}")
if [ -n "${CY_TOOLS_PATHS:-}" ]; then
    MAKE_ARGS+=(CY_TOOLS_DIR="$CY_TOOLS_PATHS")
fi
make "${MAKE_ARGS[@]}" build

ELF="$(ls build/*/Debug/*.elf 2>/dev/null | head -1)"
[ -n "$ELF" ] || { echo "no .elf in build/*/Debug/ — build first" >&2; exit 1; }

# CM4-only raw image: drop the CM0+ prebuilt and the high signature slot so the
# binary spans only the actual CM4 code/data (not the whole region). Then stamp
# (M1-M3: CRC32) or SIGN (M4: ECDSA P-256), then re-address to HEX.
"$OBJCOPY" --remove-section=.cy_m0p_image --remove-section=.cy_app_signature \
    -O binary "$ELF" build/app_cm4.bin

# M4 Seam 4: set FBL_SIGN_KEY=<path to the dev private PEM> to produce a SIGNED
# image (matching an FBL built with FBL_DIGEST_ALGO=FBL_DIGEST_SHA256). The key
# MUST be the pair of the FBL's embedded public key (crypto_pubkey_dev.h), and
# FBL_SIGN_KEY_ID (default 1) MUST match CRYPTO_DEV_KEY_ID. Unset -> CRC32 stamp
# (M1-M3 / FBL in CRC32 mode). Windows Python needs native paths (cygpath).
if [ -n "${FBL_SIGN_KEY:-}" ]; then
    _script="$(cygpath -w "$ROOT/host_tools/sign_image.py" 2>/dev/null || echo "$ROOT/host_tools/sign_image.py")"
    _key="$(cygpath -w "$FBL_SIGN_KEY" 2>/dev/null || echo "$FBL_SIGN_KEY")"
    python "$_script" build/app_cm4.bin build/app_stamped.bin \
        --key "$_key" --key-id "${FBL_SIGN_KEY_ID:-1}" --header-offset "$HEADER_OFFSET"
    _mode="SIGNED (ECDSA P-256, key_id=${FBL_SIGN_KEY_ID:-1}) — needs a SHA-256 FBL"
else
    _script="$(cygpath -w "$ROOT/host_tools/fbl_image_stamp.py" 2>/dev/null || echo "$ROOT/host_tools/fbl_image_stamp.py")"
    python "$_script" build/app_cm4.bin build/app_stamped.bin --header-offset "$HEADER_OFFSET"
    _mode="CRC32-stamped — needs a CRC32 FBL"
fi
unset _script _key
"$OBJCOPY" -I binary -O ihex --change-addresses "$APP_BASE" \
    build/app_stamped.bin build/app_stamped.hex

echo ""
echo "==> node_a_gateway/app/build/app_stamped.hex  ($_mode; @ $APP_BASE)"

if [ "$ACTION" = "build" ]; then
    echo "    Flash it: ./zz_build_gateway_app.sh program   (SWD, KitProg3)"
    echo "         or: python host_tools/uds_flash/uds_flash.py node_a_gateway/app/build/app_stamped.hex"
    echo "             (over CAN: the FBL, in programming mode, downloads + verifies it)"
    exit 0
fi

# 5. Flash. Refuse a stamped hex older than the ELF: that is the Sat 3
# failure (runbook #18), a stale image flashed after a stamp that didn't run.
if [ ! build/app_stamped.hex -nt "$ELF" ]; then
    echo "app_stamped.hex is older than $ELF -- the stamp step did not run. Not flashing." >&2
    exit 1
fi
if [ -n "${NODE_A_PROBE:-}" ]; then
    MAKE_ARGS+=(MTB_PROBE_SERIAL="$NODE_A_PROBE")
    echo "==> flashing on probe $NODE_A_PROBE"
else
    echo "WARNING: NODE_A_PROBE unset -- flashing the FIRST probe found. With both kits" >&2
    echo "         plugged in that may be Node B. Set it in bench.env (see bench.env.example)." >&2
fi
_hex="$(cygpath -m "$PWD/build/app_stamped.hex" 2>/dev/null || echo "$PWD/build/app_stamped.hex")"
make "${MAKE_ARGS[@]}" qprogram PROG_FILE="$_hex"
echo "==> flashed $_hex ($_mode) and reset; the FBL verifies it and jumps."
