#!/usr/bin/env python3
"""Fail when a change touches a protected path (docs/design/agent-pipeline.md section 7).

    python host_tools/protected_paths.py --changed changed.txt [--approved]

`changed.txt` lists the changed files, one per line; for a renamed file, list both names.
`--approved` means the owner added the label `protected-change-approved`.

Exit code: 0 = no protected path, or approved; 1 = a protected path without approval;
2 = no changed files were given (the list step broke, so the check fails closed).

The patterns live in .github/pipeline/protected-paths.txt. The CI job runs this script from
the base branch, so a pull request cannot weaken the check that judges it.
"""
import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_PATTERNS = REPO_ROOT / ".github" / "pipeline" / "protected-paths.txt"
APPROVAL_LABEL = "protected-change-approved"


def load_patterns(path):
    patterns = []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append(line)
    return patterns


def _regex(pattern):
    """Turn one pattern into a regex over a repo-relative POSIX path."""
    if pattern.endswith("/"):
        return re.compile(re.escape(pattern) + ".+")
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out))


def protected(path, patterns):
    """True when the repo-relative path matches one of the patterns."""
    path = path.strip().replace("\\", "/")
    return any(_regex(p).fullmatch(path) for p in patterns)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--changed", required=True, help="file with one changed path per line")
    parser.add_argument("--approved", action="store_true",
                        help=f"the pull request has the label {APPROVAL_LABEL}")
    parser.add_argument("--patterns", default=str(DEFAULT_PATTERNS),
                        help="pattern file (default: .github/pipeline/protected-paths.txt)")
    args = parser.parse_args(argv)

    patterns = load_patterns(args.patterns)
    lines = pathlib.Path(args.changed).read_text(encoding="utf-8").splitlines()
    changed = sorted({line.strip() for line in lines if line.strip()})
    if not changed:
        print("error: no changed files were given, so the check cannot judge the change.")
        return 2

    touched = [path for path in changed if protected(path, patterns)]
    if not touched:
        print(f"OK: no protected path among {len(changed)} changed file(s).")
        return 0

    for path in touched:
        print(f"protected: {path}")
    if args.approved:
        print(f"OK: {len(touched)} protected path(s), approved by the label {APPROVAL_LABEL}.")
        return 0
    print(f"FAIL: {len(touched)} protected path(s). Only the owner may approve this, "
          f"by adding the label {APPROVAL_LABEL}.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
