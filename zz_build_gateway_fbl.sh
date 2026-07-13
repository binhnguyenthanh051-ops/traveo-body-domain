#!/usr/bin/env bash
#
# Build the Node A gateway flash bootloader (FBL) image with ModusToolbox.
#
# Since M4 Seam 0 (ADR-0017 D4), node_a_gateway/bootloader is a two-PROJECT MTB
# APPLICATION, not a single COMBINED project:
#   proj_cm4/   the FBL itself (was directly under bootloader/ through M1-M3)
#   proj_cm0p/  the crypto-service CM0+ image (Seam 0 scope: starts the CM4, idles)
# Shared source is still pulled from ../../../shared by each project's own
# Makefile (ADR-0004); each project also resolves MTB libraries into the same
# ../../../mtb_shared.
#
# `make` at the bootloader/ (app) root is meant to fan out to both projects for
# build/program/getlibs/clean, per MTB's multi-project convention -- see
# docs/briefs/M4-bringup-plan.md Seam 0. THAT FAN-OUT IS UNVALIDATED: no
# ModusToolbox install was available when the split was scaffolded. If it
# doesn't behave as expected on your machine, scope to one project directly
# with the second argument below instead of guessing at the app-root behavior.
#
# Usage:
#   ./zz_build_gateway_fbl.sh                # build both projects (default)
#   ./zz_build_gateway_fbl.sh program         # build + flash both over KitProg3
#   ./zz_build_gateway_fbl.sh getlibs         # resolve MTB libraries for both projects
#   ./zz_build_gateway_fbl.sh clean
#   ./zz_build_gateway_fbl.sh build  cm4      # scope to proj_cm4 only (faster iteration)
#   ./zz_build_gateway_fbl.sh build  cm0p     # scope to proj_cm0p only
#   ./zz_build_gateway_fbl.sh program cm0p    # flash just the CM0+ image (its own flash
#                                              # region only -- doesn't touch the FBL's)
#
# Run from a ModusToolbox shell (or with CY_TOOLS_PATHS exported) so MTB's make
# and GCC_ARM toolchain are on PATH.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BOOTLOADER="$ROOT/node_a_gateway/bootloader"

ACTION="${1:-build}"
PROJECT="${2:-}"
MAKE_TARGET="$ACTION"

case "$PROJECT" in
    cm4)  cd "$BOOTLOADER/proj_cm4" ;;
    cm0p) cd "$BOOTLOADER/proj_cm0p" ;;
    "")   cd "$BOOTLOADER" ;;   # app root -- intended to fan out to both projects
    *)    echo "Unknown project '$PROJECT' -- use 'cm4', 'cm0p', or omit for both" >&2
          exit 1 ;;
esac

if [ -n "$PROJECT" ]; then
    case "$ACTION" in
        build|qbuild|program|qprogram|clean)
            MAKE_TARGET="${ACTION}_proj"
            ;;
    esac
fi

make "$MAKE_TARGET"
