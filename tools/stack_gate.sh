#!/usr/bin/env bash
#
# stack_gate.sh -- the stack gate of ADR-0025 D6, sourced by the zz_build_*_app.sh scripts.
#
#   stack_gate <project dir> <elf, relative to it>
#
# Runs stack_bound (tools/stack_bound) on a freshly built app image with the project's
# stack_bound.toml, writes build/stack_bound.{txt,json,html} next to the ELF, and EXITS
# the calling script unless every analysed stack passes. There is no override: a failing
# stack is fixed or raised, never flashed. The build must have the analysis flags
# (ADR-0025 D7, in the Makefile CFLAGS).
#
# The verdict comes from the JSON report, not the exit code alone: a Python crash also
# exits 1, which must never read as "a stack fails" (or as anything but "no result").

stack_gate() {
    local dir="$1" elf="$2"
    local src="$ROOT/tools/stack_bound/src"
    src="$(cygpath -w "$src" 2>/dev/null || echo "$src")"
    echo "==> stack gate (ADR-0025 D6): $dir/$elf"

    if ! python -c "import elftools, tomllib" 2>/dev/null; then
        echo "STACK GATE: $(command -v python) can't import pyelftools (or is older than 3.11) -- not continuing." >&2
        echo "    Install: python -m pip install pyelftools capstone" >&2
        echo "    (Git Bash child shells can lose APPDATA, which hides user-site packages: run with" >&2
        echo "     env APPDATA=\"\$(cygpath -w \"\$APPDATA\")\" ./zz_build_... if pip says it's installed.)" >&2
        exit 1
    fi

    rm -f "$dir/build/stack_bound.txt" "$dir/build/stack_bound.json" "$dir/build/stack_bound.html"
    local rc=0
    (cd "$dir" && PYTHONPATH="$src" python -m stack_bound "$elf" --su build --project stack_bound.toml \
        --json build/stack_bound.json --html build/stack_bound.html > build/stack_bound.txt) || rc=$?

    local result="none" json="$dir/build/stack_bound.json"
    if [ -f "$json" ]; then
        json="$(cygpath -w "$json" 2>/dev/null || echo "$json")"     # Windows Python, Cygwin/MSYS path
        result="$(python -c "import json,sys; print(json.load(open(sys.argv[1], encoding='utf-8'))['result'])" \
                  "$json" 2>/dev/null || echo unreadable)"
        result="${result%$'\r'}"                                      # Windows Python prints CRLF
    fi
    [ -s "$dir/build/stack_bound.txt" ] && sed -n '4,/^$/p' "$dir/build/stack_bound.txt"   # per-stack table

    if [ "$rc" -eq 0 ] && [ "$result" = "pass" ]; then
        echo "    stacks ok; report: $dir/build/stack_bound.html"
    elif [ "$rc" -eq 1 ] && [ "$result" = "fail" ]; then
        echo "STACK GATE: a stack fails its margin -- not continuing. See $dir/build/stack_bound.html" >&2
        exit 1
    elif [ "$result" = "analysis error" ]; then
        echo "STACK GATE: stack_bound has no result (analysis error above) -- not continuing." >&2
        exit 1
    else
        echo "STACK GATE: stack_bound did not finish (exit $rc, result '$result') -- not continuing." >&2
        exit 1
    fi
}
