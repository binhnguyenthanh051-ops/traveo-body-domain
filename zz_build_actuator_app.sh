#!/usr/bin/env bash
#
# Build the Node B actuator APPLICATION with ModusToolbox.
#
# Since M5 Seam 0, node_b_actuator/ is a two-PROJECT MTB APPLICATION:
#   proj_cm0p/  the CM0+ security core (starts CM7_0, hosts the MAC service)
#   proj_cm7/   the CM7_0 actuator application
# Shared source is still pulled from ../../shared by each project's own
# Makefile (ADR-0004); both projects also resolve MTB libraries into the same
# ../../mtb_shared cache as the rest of the repo.
#
# `make` at the node_b_actuator/ root is meant to fan out to both projects for
# build/program/getlibs/clean, per MTB's multi-project convention. Scope to a
# single project with the second argument when iterating on one core only.
#
# Usage:
#   ./zz_build_actuator_app.sh                # build both projects (default)
#   ./zz_build_actuator_app.sh program        # build + flash both over KitProg3
#   ./zz_build_actuator_app.sh getlibs        # resolve MTB libraries for both projects
#   ./zz_build_actuator_app.sh clean
#   ./zz_build_actuator_app.sh build  cm7     # scope to proj_cm7 only
#   ./zz_build_actuator_app.sh build  cm0p    # scope to proj_cm0p only
#   ./zz_build_actuator_app.sh program cm7    # flash only the CM7 image
#   ./zz_build_actuator_app.sh program cm0p   # flash only the CM0+ image
#
# Run from a ModusToolbox shell (or with CY_TOOLS_PATHS exported) so MTB's make
# and GCC_ARM toolchain are on PATH.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP="$ROOT/node_b_actuator"

ACTION="${1:-build}"
PROJECT="${2:-}"
MAKE_TARGET="$ACTION"

case "$PROJECT" in
    cm7)  cd "$APP/proj_cm7" ;;
    cm0p) cd "$APP/proj_cm0p" ;;
    "")   cd "$APP" ;;
    *)    echo "Unknown project '$PROJECT' -- use 'cm7', 'cm0p', or omit for both" >&2
          exit 1 ;;
esac

if [ -n "$PROJECT" ]; then
    case "$ACTION" in
        build|qbuild|program|qprogram|clean)
            MAKE_TARGET="${ACTION}_proj"
            ;;
    esac
fi

# Pass CY_TOOLS_DIR directly so $(wildcard C:/...) in the MTB Makefiles resolves
# correctly under Git Bash's make, which does not expand C:/ wildcard paths.
if [ -n "${CY_TOOLS_PATHS:-}" ]; then
    make CY_TOOLS_DIR="$CY_TOOLS_PATHS" "$MAKE_TARGET"
else
    make "$MAKE_TARGET"
fi

# The MTB application recipe merges the CM0+ + CM7 project hexes into
# build/app_combined.hex. Publish a clearer copy for flashing/reference at the
# application root only, where both projects are merged.
if [ -z "$PROJECT" ]; then
    COMBINED="$APP/build/app_combined.hex"
    if [ -f "$COMBINED" ]; then
        cp -f "$COMBINED" "$APP/build/actuator_app_combined.hex"
        echo "Merged actuator image: node_b_actuator/build/actuator_app_combined.hex"
    fi
fi
