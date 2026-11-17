#!/usr/bin/env bash
#
# Start OpenOCD as a GDB server on one kit (no reset, no program): attach GDB to it.
# Attach only: no reset, no program, no erase (docs/workflow.md "Bench rule"). Ctrl+C stops it.
#
# Usage:
#   bash ./tools/bench/gdbserver_node.sh A  # Node A (gateway, CYT2B7)
#   bash ./tools/bench/gdbserver_node.sh B  # Node B (actuator, CYT4BF): CM0+ on 3333, CM7_0 on 3334 (check the startup lines)
#
# The probe comes from bench.env (NODE_A_PROBE / NODE_B_PROBE) and must be set:
# with both kits plugged in, an unpinned probe could attach to the wrong board.
#
# Used by tools/bench/stack_demo_capture.gdb (the STACK_DEMO=1 image). Attaching halts
# the core while GDB holds it; `detach` lets it run on.

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
echo "==> GDB server for Node $1 on probe $SERIAL"
# ENABLE_ACQUIRE 0: the kit's default is to XRES-reset and "acquire" the chip in test mode at
# init, which stops the running app (bench 2026-11-16). Off, OpenOCD attaches to it as it runs.
"$OPENOCD" -s "$_scripts" -c "source [find interface/kitprog3.cfg]; adapter serial $SERIAL; \
transport select swd; set ENABLE_ACQUIRE 0; source [find $TARGET]; init"
