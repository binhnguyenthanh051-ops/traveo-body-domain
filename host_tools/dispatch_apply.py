#!/usr/bin/env python3
"""Apply a Dispatcher decision (host_tools/dispatcher.py): change the CR's labels, and print
the decision as GitHub Actions outputs (issue=, start=, line=).

    python host_tools/dispatch_apply.py decision.json >> "$GITHUB_OUTPUT"

It changes labels and nothing else: the Dispatcher never pushes code (design note section 3).
"""
import json
import pathlib
import subprocess
import sys


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    argv = sys.argv[1:] if argv is None else argv
    d = json.loads(pathlib.Path(argv[0]).read_text(encoding="utf-8"))
    if d["issue"] and (d["add"] or d["remove"]):
        args = ["issue", "edit", str(d["issue"])]
        for label in d["add"]:
            args += ["--add-label", label]
        for label in d["remove"]:
            args += ["--remove-label", label]
        gh(args)
    # One line per output: a newline in the status line must not start another output.
    line = " ".join(d["line"].split())
    print(f"issue={d['issue'] or ''}")
    print(f"start={d['start'] or ''}")
    print(f"line={line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
