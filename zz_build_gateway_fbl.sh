#!/usr/bin/env bash
#
# Build the Node A gateway flash bootloader (FBL) image with ModusToolbox.
# One thin wrapper per variant; the variant's own Makefile/linker/build live in
# node_a_gateway/bootloader. Shared source is pulled from ../../shared by that
# Makefile (ADR-0004).
#
# Usage:
#   ./zz_build_gateway_fbl.sh            # build (default)
#   ./zz_build_gateway_fbl.sh program    # build + flash over KitProg3
#   ./zz_build_gateway_fbl.sh getlibs    # resolve MTB libraries into ../mtb_shared
#   ./zz_build_gateway_fbl.sh clean
#
# Run from a ModusToolbox shell (or with CY_TOOLS_PATHS exported) so MTB's make
# and GCC_ARM toolchain are on PATH.

set -euo pipefail
cd "$(dirname "$0")/node_a_gateway/bootloader"
make "${@:-build}"
