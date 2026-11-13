#!/usr/bin/env python3
"""The Builder's work queue, and the "did the owner set this label" check.

    python host_tools/cr_queue.py pick --owner <login>
    python host_tools/cr_queue.py label-by-owner --owner <login> --issue <n> --label <name>

`pick` prints JSON for the workflow: {"issue": <number or null>, "skipped": [[n, reason]]}.
It takes the oldest open issue labelled `ready` that is agent-ready by its metadata
(docs/design/agent-pipeline.md section 3): written by the owner, typed `cr` or `bug`, and
`ready` added by the owner. It reads labels, authors and timelines, never issue text: an empty
form field is the Builder's to report, as `blocked`. It only reads GitHub.

`label-by-owner` exits 0 when the label is on the issue (or pull request) and the owner
added it last, 1 otherwise. An app token can add labels, so the presence of a label is not
the owner's approval; its timeline is (ADR-0026).

Both talk to GitHub through the `gh` CLI, in the repository of the current directory.
"""
import argparse
import json
import subprocess
import sys

ISSUES_API = "repos/{owner}/{repo}/issues"

def last_labeler(events, label):
    """Who added the label last, or None when it was removed after its last add."""
    who = None
    for event in events:
        if event.get("label", {}).get("name") != label:
            continue
        if event.get("event") == "labeled":
            who = event["actor"]["login"]
        elif event.get("event") == "unlabeled":
            who = None
    return who


def timeline(gh, number):
    return json.loads(gh(["api", "--paginate", f"{ISSUES_API}/{number}/timeline?per_page=100"]))


def pick(gh, owner):
    """Return (issue number or None, [(number, reason) for each ready issue skipped])."""
    listed = json.loads(gh(["api", f"{ISSUES_API}?state=open&labels=ready"
                                   "&sort=created&direction=asc&per_page=100"]))
    skipped = []
    for item in sorted(listed, key=lambda i: (i["created_at"], i["number"])):
        if "pull_request" in item:
            continue
        number = item["number"]
        names = {label["name"] for label in item["labels"]}
        kinds = [k for k in ("cr", "bug") if k in names]
        if item["user"]["login"] != owner:
            skipped.append((number, "not written by the owner"))
        elif len(kinds) != 1:
            skipped.append((number, "neither cr nor bug" if not kinds else "both cr and bug"))
        elif (who := last_labeler(timeline(gh, number), "ready")) != owner:
            skipped.append((number, f"ready was set by {who}, not the owner"))
        else:
            return number, skipped
    return None, skipped


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_pick = sub.add_parser("pick", help="print the CR the Builder takes next, as JSON")
    p_pick.add_argument("--owner", required=True, help="the owner's GitHub login")
    p_label = sub.add_parser("label-by-owner", help="exit 0 if the owner added the label")
    p_label.add_argument("--owner", required=True)
    p_label.add_argument("--issue", required=True, type=int)
    p_label.add_argument("--label", required=True)
    args = parser.parse_args(argv)

    if args.command == "pick":
        number, skipped = pick(gh, args.owner)
        print(json.dumps({"issue": number, "skipped": [list(s) for s in skipped]}))
        return 0

    who = last_labeler(timeline(gh, args.issue), args.label)
    if who == args.owner:
        print(f"OK: '{args.label}' on #{args.issue} was added by the owner.")
        return 0
    print(f"FAIL: '{args.label}' on #{args.issue} was " +
          (f"added by {who}, not the owner." if who else "not added (or was removed)."))
    return 1


if __name__ == "__main__":
    sys.exit(main())
