#!/usr/bin/env bash
# stop_gate.sh — Claude Code Stop hook: the host quality gate (brief Q1).
#
# Claude may not hand back a turn while `make test && make lint` is red.
#
# Lifecycle: when Claude finishes answering, Claude Code fires `Stop` and runs
# this script with the hook input as JSON on stdin. The EXIT CODE decides:
#   exit 0  -> the stop is allowed. Plain stdout goes to the debug log only, so
#              any message meant for a human is printed as {"systemMessage":..}.
#   exit 2  -> the stop is BLOCKED; stderr is handed to Claude as the reason and
#              the turn continues, so Claude fixes the failure before handing back.
#   other   -> a non-blocking error; the stop still happens. Never used on purpose.
#
# Design decisions (agreed 2026-09-26, docs/briefs/Q1-stop-hook-brief.md):
#  1. Only when it matters: runs only if the turn changed *.c, *.h, a Makefile
#     or a tests/ file — measured against the turn's START commit recorded by
#     turn_start.sh (so a turn that commits its own work is still gated), plus
#     untracked files. A docs-only turn exits in milliseconds.
#  2. Loop guard: our own per-turn counter, not stop_hook_active alone (which
#     would allow exactly one retry). Blocks at most MAX_BLOCKS times, then lets
#     the stop happen and says plainly that the gate is still red.
#  3. Never blocks on a missing toolchain (cloud sessions, a fresh laptop):
#     warns visibly and allows the stop.
#  4. Two traps the gate would otherwise lie about:
#     - msys `make` strips TMP/TEMP from its recipes, so gcc falls back to
#       C:\WINDOWS\ and dies. Fixed by passing TMP/TEMP on the make command line
#       (command-line variables DO reach recipes). Windows only.
#     - Test binaries depend on their .c files, not headers: after a header-only
#       change `make test` re-runs STALE binaries and reports green. So any .h or
#       Makefile change forces a full rebuild (`make -B`, ~27 s cold).
#  5. Short output: failing test names, the first compile/lint errors, and the
#     path of the full log — not 500 lines.

set -u

MAX_BLOCKS=3

cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
input="$(cat)"

# JSON-escape stdin (awk only: jq is not guaranteed to exist).
json_escape() {
    awk '{ gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); gsub(/\t/, "\\t"); gsub(/\r/, "");
           printf "%s\\n", $0 }'
}
# A message the HUMAN sees, while still allowing the stop (exit 0).
say() {
    printf '{"systemMessage":"%s"}\n' "$(printf '%s' "$1" | json_escape)"
}

session="$(printf '%s' "$input" \
    | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    | head -n 1 | tr -cd 'A-Za-z0-9_-')"
[ -n "$session" ] || session="nosession"

command -v git >/dev/null 2>&1 || { say "Stop gate skipped: git not found."; exit 0; }
git rev-parse --git-dir >/dev/null 2>&1 || exit 0        # not a repo: nothing to gate
state="$(git rev-parse --git-dir)/claude-gate"
mkdir -p "$state/tmp" 2>/dev/null || exit 0
count_file="$state/$session.count"

# ---- 1. did this turn touch source? --------------------------------------
base="$(cat "$state/$session.base" 2>/dev/null)"
if [ -z "$base" ] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    base="HEAD"                        # no baseline recorded: best effort
fi
relevant="$( { git diff --name-only "$base" -- ; git ls-files --others --exclude-standard; } \
    2>/dev/null | sort -u | grep -E '\.(c|h)$|(^|/)Makefile$|(^|/)tests/' )"
if [ -z "$relevant" ]; then
    rm -f "$count_file"
    exit 0                             # docs-only turn: stop instantly
fi

# ---- 3. toolchain present? ------------------------------------------------
missing=""
for t in make cc cppcheck; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    say "Stop gate SKIPPED — missing:${missing}. Source changed but make test/lint did NOT run; treat this turn as unverified."
    exit 0
fi

# ---- 4. build the make invocation ------------------------------------------
make_vars=()
case "$(uname -s 2>/dev/null)" in
    MINGW* | MSYS* | CYGWIN*)
        tmpw="$(cygpath -w "$state/tmp" 2>/dev/null)"
        [ -n "$tmpw" ] && make_vars=("TMP=$tmpw" "TEMP=$tmpw")
        ;;
esac
force=""
if printf '%s\n' "$relevant" | grep -qE '\.h$|(^|/)Makefile$'; then
    force="-B"                         # headers aren't prerequisites: rebuild all
fi

# ---- run the gate ----------------------------------------------------------
test_log="$state/last-test.log"
lint_log="$state/last-lint.log"
make $force -k ${make_vars[@]+"${make_vars[@]}"} test >"$test_log" 2>&1
test_rc=$?
make lint >"$lint_log" 2>&1
lint_rc=$?

if [ "$test_rc" -eq 0 ] && [ "$lint_rc" -eq 0 ]; then
    rm -f "$count_file"
    exit 0                             # green: hand back
fi

# ---- 5. red: a short summary -----------------------------------------------
summary=""
if [ "$test_rc" -ne 0 ]; then
    fails="$(grep -E ':FAIL' "$test_log" | head -n 10)"
    # Compiler format is "file:line:col: error: ...". Match that exactly: a loose
    # 'error:' also hits Unity lines for tests NAMED *_error that PASSED.
    errs="$(grep -E ': (fatal )?error:|Cannot create temporary file|Segmentation fault|Aborted' "$test_log" \
        | grep -vE ':(PASS|FAIL)' | head -n 10)"
    summary="make test FAILED (exit $test_rc${force:+, full rebuild because a header/Makefile changed}):"
    [ -n "$fails" ] && summary="$summary
Failing tests:
$fails"
    [ -n "$errs" ] && summary="$summary
Build/runtime errors:
$errs"
    if [ -z "$fails" ] && [ -z "$errs" ]; then
        summary="$summary
Last lines of the log:
$(tail -n 5 "$test_log")"
    fi
    summary="$summary
Full log: $test_log"
fi
if [ "$lint_rc" -ne 0 ]; then
    lint_errs="$(grep -E '(error|warning|style|portability|performance):' "$lint_log" | head -n 10)"
    [ -n "$lint_errs" ] || lint_errs="$(tail -n 5 "$lint_log")"
    summary="${summary:+$summary

}make lint FAILED (exit $lint_rc):
$lint_errs
Full log: $lint_log"
fi

# ---- 2. loop guard -----------------------------------------------------------
n="$(cat "$count_file" 2>/dev/null)"
case "$n" in '' | *[!0-9]*) n=0 ;; esac
n=$((n + 1))
if [ "$n" -gt "$MAX_BLOCKS" ]; then
    rm -f "$count_file"
    say "GATE STILL RED after $MAX_BLOCKS attempts — handing back anyway so a human can look.
$summary"
    exit 0
fi
printf '%s\n' "$n" >"$count_file"

printf 'Host gate is RED (attempt %s of %s). Fix this before handing back:\n%s\n' \
    "$n" "$MAX_BLOCKS" "$summary" >&2
exit 2
