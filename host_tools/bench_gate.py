#!/usr/bin/env python3
"""bench_gate.py -- post the `bench` commit status CI owes every pull request (build step 8).

    python host_tools/bench_gate.py --branch <head ref> --sha <head sha>

`bench` is a required check on main, so every pull request needs it. This posts it:
success for a non-CR branch and for a host-only CR, pending for an on-target CR (`level:on-target`
or `level:both`) until host_tools/bench_run.py posts the real result on the same commit. A CR
with no level label stays pending: the owner sets the level at gate 1b. It reads labels only.
"""
import argparse
import json
import re
import subprocess
import sys

CONTEXT = "bench"


def decide(branch, labels):
    """Return (state, description) for the bench status."""
    if not re.fullmatch(r"cr/\d+", branch or ""):
        return "success", "not a CR: no bench run needed"
    labels = set(labels)
    if labels & {"level:on-target", "level:both"}:
        return "pending", "on-target CR: waiting for the bench run"
    if "level:host" in labels:
        return "success", "host-only CR: no bench run needed"
    return "pending", "no level label: the owner sets level:* at gate 1b"


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--branch", required=True)
    parser.add_argument("--sha", required=True)
    args = parser.parse_args(argv)

    labels = []
    m = re.fullmatch(r"cr/(\d+)", args.branch)
    if m:
        labels = [l["name"] for l in json.loads(
            gh(["api", f"repos/{{owner}}/{{repo}}/issues/{m.group(1)}/labels"]))]
    state, why = decide(args.branch, labels)
    gh(["api", "--method", "POST", f"repos/{{owner}}/{{repo}}/statuses/{args.sha}",
        "-f", f"state={state}", "-f", f"context={CONTEXT}", "-f", f"description={why}"])
    print(f"bench: {state} ({why})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
