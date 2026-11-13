#!/usr/bin/env python3
"""The three numbers per CR (docs/design/agent-pipeline.md section 8).

    python host_tools/cr_numbers.py --owner <login> [--milestone <title>] [--json]

For every closed issue labelled `done`, it prints:

- rework rounds: the highest `round:N` label the CR ever had, or how many times `rework`
  was added if that is higher (a CR walked through by hand has no round label);
- ready to merge: from the owner's first `ready` to the merge, so the time spent waiting for
  the owner as referee counts;
- referee calls: how many times `waiting-for-me` was added.

All three come from the label events in the issue timeline, never from issue or comment
text. The Dispatcher logs them on the status line when a CR's pull request is merged
(routing row 13); this script recomputes them at any time, for the blog. It only reads GitHub.
"""
import argparse
import datetime
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cr_queue  # noqa: E402

ROUND = re.compile(r"round:(\d+)")


def _when(stamp):
    return datetime.datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def numbers(events, owner, merged_at=None):
    """The three numbers from an issue timeline. `merged_at` wins over the `closed` event."""
    added = [(e["label"]["name"], e["actor"]["login"], e["created_at"])
             for e in events if e.get("event") == "labeled" and "label" in e]
    rounds = [int(m.group(1)) for name, _, _ in added if (m := ROUND.fullmatch(name))]
    reworks = sum(1 for name, _, _ in added if name == "rework")
    ready = next((when for name, who, when in added if name == "ready" and who == owner), None)
    if merged_at is None:
        closed = [e["created_at"] for e in events if e.get("event") == "closed"]
        merged_at = closed[-1] if closed else None
    minutes = None
    if ready and merged_at:
        minutes = round((_when(merged_at) - _when(ready)).total_seconds() / 60)
    return {"rework_rounds": max([reworks, *rounds]),
            "ready_to_merge_min": minutes,
            "referee_calls": sum(1 for name, _, _ in added if name == "waiting-for-me")}


def duration(minutes):
    if minutes is None:
        return "n/a"
    hours, rest = divmod(minutes, 60)
    return f"{hours}h{rest:02d}m" if hours else f"{rest}m"


def line(n):
    """The numbers as the Dispatcher writes them on the status line."""
    return (f"numbers: rework rounds {n['rework_rounds']}, "
            f"ready to merge {duration(n['ready_to_merge_min'])}, "
            f"referee calls {n['referee_calls']}")


def done_issues(gh, milestone=None):
    listed = json.loads(gh(["api", "--paginate", f"{cr_queue.ISSUES_API}?state=closed"
                                                 "&labels=done&per_page=100"]))
    return sorted((i for i in listed if "pull_request" not in i
                   and (milestone is None or (i.get("milestone") or {}).get("title") == milestone)),
                  key=lambda i: i["number"])


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--owner", required=True, help="the owner's GitHub login")
    parser.add_argument("--milestone", default=None, help="only CRs in this milestone (title)")
    parser.add_argument("--json", action="store_true", help="print JSON instead of a table")
    args = parser.parse_args(argv)

    rows = [{"issue": i["number"], "title": i["title"],
             **numbers(cr_queue.timeline(gh, i["number"]), args.owner)}
            for i in done_issues(gh, args.milestone)]
    if args.json:
        print(json.dumps(rows, indent=2))
        return 0
    print("| CR | Rework rounds | Ready to merge | Referee calls |")
    print("|---|---|---|---|")
    for r in rows:
        print(f"| #{r['issue']} {r['title']} | {r['rework_rounds']} | "
              f"{duration(r['ready_to_merge_min'])} | {r['referee_calls']} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
