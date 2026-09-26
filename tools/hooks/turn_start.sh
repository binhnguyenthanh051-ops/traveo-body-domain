#!/usr/bin/env bash
# turn_start.sh — Claude Code UserPromptSubmit hook (brief Q1).
#
# Records where this turn STARTED, so the Stop hook can tell whether the turn
# touched source. Diffing against HEAD at stop time is not enough: a turn that
# commits its own work leaves a clean tree and would skip the gate on exactly
# the turn that changed the most code.
#
# Also resets the Stop hook's retry counter: the 3-block cap is per turn.
#
# MUST stay silent on stdout — for UserPromptSubmit, stdout is injected into
# Claude's context. Never blocks: every path exits 0.

set -u

cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
input="$(cat)"

session="$(printf '%s' "$input" \
    | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    | head -n 1 | tr -cd 'A-Za-z0-9_-')"
[ -n "$session" ] || session="nosession"

git rev-parse --git-dir >/dev/null 2>&1 || exit 0
state="$(git rev-parse --git-dir)/claude-gate"    # inside .git: never tracked
mkdir -p "$state" 2>/dev/null || exit 0

# An empty repo has no HEAD; the Stop hook then falls back to HEAD itself.
git rev-parse --verify -q HEAD >"$state/$session.base" 2>/dev/null
rm -f "$state/$session.count"

# Housekeeping: one tiny file per session accumulates; drop week-old ones.
find "$state" -maxdepth 1 -type f \( -name '*.base' -o -name '*.count' \) \
    -mtime +7 -exec rm -f {} + 2>/dev/null

exit 0
