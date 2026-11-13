#!/usr/bin/env python3
"""Publish the Planner's plan as draft issues (docs/design/agent-pipeline.md section 3, rows 14-17).

    python host_tools/planner_publish.py plan.json --source <issue> >> "$GITHUB_OUTPUT"

The Planner (an agent) writes plan.json:
    {"status": "done" | "blocked",
     "summary": "<one paragraph for the owner>",
     "issues": [{"kind": "cr" | "bug", "title": "...", "fields": {"<form heading>": "<value>"}}]}

This script checks the plan against the issue-form contract (.github/ISSUE_TEMPLATE/): every
field of the form, filled, and nothing else. Only a valid plan becomes issues, each labelled
`draft` and its kind, in its milestone. Then one comment on the source issue lists them, with
their acceptance criteria: the list the owner approves at gate 1b. An invalid or blocked plan
creates nothing; the comment says why. Prints `result=done|blocked|failed`.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

# The "### <label>" headings the issue forms render, in form order.
FIELDS = {
    "cr": ["Type", "What and why", "ADR link", "Requirement IDs", "Acceptance criteria",
           "Test level", "Milestone"],
    "bug": ["Observed", "Expected", "How to reproduce", "Affected requirement",
            "Test level", "Milestone"],
}
LEVELS = {"host", "on-target", "both"}
MAX_ISSUES = 15


def validate(plan):
    """Return a list of problems; empty when the plan can be published."""
    problems = []
    if plan.get("status") != "done":
        return problems
    issues = plan.get("issues") or []
    if not issues:
        problems.append("a done plan needs at least one issue")
    if len(issues) > MAX_ISSUES:
        problems.append(f"at most {MAX_ISSUES} issues per plan, got {len(issues)}")
    for n, item in enumerate(issues, 1):
        where = f"issue {n}"
        kind = item.get("kind")
        if kind not in FIELDS:
            problems.append(f"{where}: kind must be cr or bug")
            continue
        if not (item.get("title") or "").strip():
            problems.append(f"{where}: no title")
        fields = item.get("fields") or {}
        for name in FIELDS[kind]:
            if name not in fields:
                problems.append(f"{where}: missing field '{name}'")
            elif not str(fields[name]).strip():
                problems.append(f"{where}: empty field '{name}'")
        for name in fields:
            if name not in FIELDS[kind]:
                problems.append(f"{where}: unknown field '{name}'")
        if str(fields.get("Test level", "")).strip() not in LEVELS | {""}:
            problems.append(f"{where}: Test level must be host, on-target or both")
    return problems


def body(item):
    return "\n".join(f"### {name}\n\n{str(item['fields'][name]).strip()}\n"
                     for name in FIELDS[item["kind"]])


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def _comment(gh, source, text):
    gh(["issue", "comment", str(source), "--body-file", "-"], stdin=text)


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("plan")
    parser.add_argument("--source", required=True, type=int, help="the issue that was planned")
    args = parser.parse_args(argv)

    try:
        plan = json.loads(pathlib.Path(args.plan).read_text(encoding="utf-8"))
    except (OSError, ValueError) as err:
        _comment(gh, args.source, f"## Planner\n\nNo usable plan was written ({err}).")
        print("result=failed")
        return 0

    problems = validate(plan)
    if plan.get("status") != "done" or problems:
        lines = ["## Planner: nothing created", "", str(plan.get("summary", "")).strip()]
        if problems:
            lines += ["", "The plan breaks the issue-form contract:"] + [f"- {p}" for p in problems]
        _comment(gh, args.source, "\n".join(lines))
        print("result=blocked")
        return 0

    created = []
    for item in plan["issues"]:
        url = gh(["issue", "create", "--title", item["title"].strip(),
                  "--label", "draft", "--label", item["kind"],
                  "--milestone", str(item["fields"]["Milestone"]).strip(),
                  "--body-file", "-"], stdin=body(item))
        number = re.search(r"/issues/(\d+)", url).group(1)
        created.append((number, item))

    lines = ["## Planner: draft issues for gate 1b", "", str(plan.get("summary", "")).strip(), "",
             "Set `ready` and a `level:*` label on each one you approve."]
    for number, item in created:
        lines += ["", f"### #{number} {item['title'].strip()}", ""]
        key = "Acceptance criteria" if item["kind"] == "cr" else "Expected"
        lines.append(str(item["fields"][key]).strip())
    _comment(gh, args.source, "\n".join(lines))
    print("result=done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
