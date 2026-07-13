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

# In signing mode, `make program` is WRONG: it flashes MTB's raw build output
# (the UNSIGNED gateway_app.hex), which a SHA-256 FBL then rejects. The signed
# image (app_stamped.hex, built below) must be flashed via uds_flash or a
# programmer. So downgrade a 'program' request to 'build' when signing.
MAKE_ACTION="${1:-build}"
if [ -n "${FBL_SIGN_KEY:-}" ] && [ "$MAKE_ACTION" = "program" ]; then
    echo "NOTE: signing mode — 'make program' flashes the UNSIGNED image, so building only." >&2
    echo "      Flash the SIGNED app_stamped.hex (printed below) via uds_flash / your programmer." >&2
    MAKE_ACTION="build"
fi

# Pass CY_TOOLS_DIR directly so $(wildcard C:/...) in the MTB Makefile resolves
# correctly under Git Bash's make, which does not expand C:/ wildcard paths.
if [ -n "${CY_TOOLS_PATHS:-}" ]; then
    make CY_TOOLS_DIR="$CY_TOOLS_PATHS" "$MAKE_ACTION"
else
    make "$MAKE_ACTION"
fi

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
if [ -n "${FBL_SIGN_KEY:-}" ]; then
    echo "    Flash THIS (not 'make program', which flashes the unsigned image):"
    echo "      python host_tools/uds_flash/uds_flash.py node_a_gateway/app/build/app_stamped.hex"
    echo "    (the FBL, in programming mode, downloads it, verifies the signature, and jumps)"
else
    echo "    Flash this; the FBL verifies (CRC32) + jumps."
fi
