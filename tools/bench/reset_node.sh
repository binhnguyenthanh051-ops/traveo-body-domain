#!/usr/bin/env bash
#
# Reset one kit over its KitProg3 probe (OpenOCD `init; reset run; shutdown`).
# Reset ONLY: no program, no erase, no supervisory flash (docs/workflow.md "Bench rule").
#
# Usage:
#   bash ./tools/bench/reset_node.sh A      # Node A (gateway, CYT2B7)
#   bash ./tools/bench/reset_node.sh B      # Node B (actuator, CYT4BF)
#
# The probe comes from bench.env (NODE_A_PROBE / NODE_B_PROBE) and must be set:
# with both kits plugged in, an unpinned reset could hit the wrong board.
#
# This is a debug-port reset, NOT a reset-button (XRES) boot: LOG_EVT_BOOT's
# reset_reason shows the cause the debugger produced. Say which one a bench
# record used.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -f "$ROOT/bench.env" ]; then
    . "$ROOT/bench.env"
fi

# Windows path in, shell path out: PowerShell's `bash` is Cygwin (/cygdrive/c/...),
# Git Bash is MSYS (/c/...); cygpath gets both right.
PROGTOOLS_WIN="${MTB_PROGTOOLS_DIR:-C:/Infineon/Tools/ModusToolboxProgtools-1.8}"
PROGTOOLS="$(cygpath -u "$PROGTOOLS_WIN" 2>/dev/null || echo "$PROGTOOLS_WIN")"
OPENOCD="$PROGTOOLS/openocd/bin/openocd.exe"
SCRIPTS="$PROGTOOLS/openocd/scripts"

case "${1:-}" in
    A) SERIAL="${NODE_A_PROBE:-}"; TARGET="target/traveo2_1m_a0.cfg" ;;
    B) SERIAL="${NODE_B_PROBE:-}"; TARGET="target/infineon/cyt4bf.cfg" ;;
    *) echo "usage: $0 A|B" >&2; exit 2 ;;
esac

if [ -z "$SERIAL" ]; then
    echo "NODE_${1}_PROBE is not set (bench.env) -- refusing to reset an unpinned probe." >&2
    exit 1
fi
[ -x "$OPENOCD" ] || { echo "OpenOCD not found: $OPENOCD (set MTB_PROGTOOLS_DIR)" >&2; exit 1; }

_scripts="$(cygpath -m "$SCRIPTS" 2>/dev/null || echo "$SCRIPTS")"
echo "==> reset Node $1 on probe $SERIAL"
"$OPENOCD" -s "$_scripts" -c "source [find interface/kitprog3.cfg]; adapter serial $SERIAL; \
transport select swd; source [find $TARGET]; init; reset run; shutdown"
