#!/usr/bin/env python3
"""The Dispatcher: the agent pipeline's one router (docs/design/agent-pipeline.md section 3).

Deterministic, not an agent. It reads labels, events and who acted, never issue or comment
text. For each event it checks the pause switch, the author rule and the rework round, maps
the CR's state to the next step (section 6), says which agent to start, and writes one
status line, also when it does nothing.

    python host_tools/dispatcher.py decide --event-name <name> --event-path <file> --owner <login> [--paused]
    python host_tools/dispatcher.py settle --issue <n> --owner <login> (--builder <result> | --inspector <verdict>) [--paused]

Both print the decision as JSON: {"issue", "add", "remove", "start", "line"}. The workflow
applies the label changes and starts the agent job named in "start" (build, rework, inspect).
"""
import argparse
import dataclasses
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cr_queue  # noqa: E402

STATES = ("draft", "ready", "building", "review", "rework", "needs-bench",
          "awaiting-merge", "waiting-for-me", "done")
GATE_LABELS = {"ready", "rework", "draft", "protected-change-approved"}
CAP = 2                 # rework rounds before the owner referees
REFEREE_ROUND = CAP + 1  # the one extra round after the owner's answer
CR_BRANCH = re.compile(r"cr/(\d+)")


@dataclasses.dataclass
class Decision:
    issue: int | None
    add: list
    remove: list
    start: str | None    # "build" | "rework" | "inspect" | None
    line: str

    def as_dict(self):
        return dataclasses.asdict(self)


def _state(labels):
    found = [s for s in STATES if s in labels]
    return found[0] if len(found) == 1 else None


def _round(labels):
    rounds = [int(l.split(":")[1]) for l in labels if re.fullmatch(r"round:\d+", l)]
    return max(rounds, default=0)


def _decide(issue, labels, add=(), remove=(), start=None, line=""):
    add, remove = set(add), set(remove) - set(add)
    return Decision(issue, sorted(add - set(labels)), sorted(remove & set(labels)), start,
                    f"#{issue}: {line}" if issue else line)


def _nothing(issue, line):
    return Decision(issue, [], [], None, f"#{issue}: {line}" if issue else line)


def _failed(issue, labels, state, why):
    """A failed CI run, review or bench: rework, or the owner, or back to draft."""
    r = _round(labels)
    rounds = {l for l in labels if l.startswith("round:")}
    if r >= REFEREE_ROUND:
        return _decide(issue, labels, add={"draft"}, remove={state} | rounds,
                       line=f"{why} in the referee round: back to draft")
    if r >= CAP:
        return _decide(issue, labels, add={"waiting-for-me"}, remove={state},
                       line=f"{why}, rework cap ({CAP}) reached: waiting for the owner")
    return _decide(issue, labels, add={"rework", f"round:{r + 1}"}, remove={state} | rounds,
                   start="rework", line=f"{why}: rework round {r + 1} of {CAP}")


def route(event, labels, owner, paused):
    """The routing table. `labels` are the CR's labels now; returns a Decision."""
    kind, issue = event["kind"], event.get("issue")
    if paused:
        return _nothing(issue, f"paused (PIPELINE_PAUSED): {kind} event not routed")
    state = _state(labels)

    if kind == "comment":
        return _nothing(issue, f"comment by {event['actor']}: comments never route")

    if kind == "label":
        label, actor = event["label"], event["actor"]
        if actor != owner:
            return _nothing(issue, f"label '{label}' added by {actor} ignored: "
                                   "only the owner's labels route")
        if label == "rework" and "waiting-for-me" in labels:
            r = _round(labels)
            rounds = {l for l in labels if l.startswith("round:")}
            return _decide(issue, labels, add={"rework", f"round:{r + 1}"},
                           remove={"waiting-for-me"} | rounds, start="rework",
                           line=f"owner answered: one more round (round {r + 1})")
        if label == "ready":
            return _nothing(issue, "ready: queued for the next tick")
        return _nothing(issue, f"label '{label}' by the owner: nothing to route")

    if kind == "tick":
        if issue is None:
            return _nothing(None, "no ready CR")
        return _decide(issue, labels, add={"building"}, remove={"ready"}, start="build",
                       line="building")

    if kind in ("ci", "merged") and not event.get("same_repo", False):
        return _nothing(issue, f"{kind} event from a fork ignored")

    if kind == "ci":
        if state not in ("building", "rework"):
            return _nothing(issue, f"CI {event['conclusion']} ignored in state {state}")
        if event["conclusion"] == "success":
            return _decide(issue, labels, add={"review"}, remove={state}, start="inspect",
                           line="host CI green: review")
        return _failed(issue, labels, state, f"host CI {event['conclusion']}")

    if kind == "inspector":
        if state != "review":
            return _nothing(issue, f"Inspector verdict ignored in state {state}")
        if event["verdict"] != "approve":
            return _failed(issue, labels, state, "Inspector findings")
        if "level:host" in labels:
            return _decide(issue, labels, add={"awaiting-merge"}, remove={state},
                           line="approved, host only: awaiting merge")
        if "level:on-target" in labels or "level:both" in labels:
            return _decide(issue, labels, add={"needs-bench"}, remove={state},
                           line="approved: bench booked")
        return _decide(issue, labels, add={"waiting-for-me"}, remove={state},
                       line="approved, but no level label: waiting for the owner")

    if kind == "bench":
        if state != "needs-bench":
            return _nothing(issue, f"bench result ignored in state {state}")
        if event["conclusion"] == "success":
            return _decide(issue, labels, add={"awaiting-merge"}, remove={state},
                           line="bench pass: awaiting merge")
        return _failed(issue, labels, state, "bench fail")

    if kind == "builder":
        if event["result"] == "done":
            return _nothing(issue, "Builder published; host CI routes next")
        return _decide(issue, labels, add={"waiting-for-me"}, remove={state} if state else (),
                       line=f"Builder {event['result']}: waiting for the owner")

    if kind == "merged":
        clear = {s for s in STATES if s in labels} | {l for l in labels if l.startswith("round:")}
        return _decide(issue, labels, add={"done"}, remove=clear, line="merged: done")

    return _nothing(issue, f"{kind} event: nothing to route")


def _cr_number(branch):
    m = CR_BRANCH.fullmatch(branch or "")
    return int(m.group(1)) if m else None


def facts(event_name, payload):
    """GitHub event payload -> route() event, from metadata only. None: not ours."""
    if event_name in ("schedule", "workflow_dispatch"):
        return {"kind": "tick"}
    if event_name == "issues" and payload.get("action", "labeled") == "labeled":
        return {"kind": "label", "issue": payload["issue"]["number"],
                "label": payload["label"]["name"], "actor": payload["sender"]["login"]}
    if event_name == "issue_comment":
        return {"kind": "comment", "issue": payload["issue"]["number"],
                "actor": payload["sender"]["login"]}
    repo = payload.get("repository", {}).get("full_name")
    if event_name == "workflow_run":
        run = payload["workflow_run"]
        issue = _cr_number(run.get("head_branch"))
        if issue is None:
            return None
        return {"kind": "ci", "issue": issue, "conclusion": run["conclusion"],
                "same_repo": (run.get("head_repository") or {}).get("full_name") == repo}
    if event_name == "pull_request":
        pr = payload["pull_request"]
        issue = _cr_number(pr["head"]["ref"])
        if not pr.get("merged") or issue is None:
            return None
        return {"kind": "merged", "issue": issue,
                "same_repo": (pr["head"].get("repo") or {}).get("full_name") == repo}
    if event_name == "status":
        if payload.get("context") != "bench" or payload.get("state") not in ("success", "failure", "error"):
            return None
        issues = [n for b in payload.get("branches", []) if (n := _cr_number(b["name"]))]
        if not issues:
            return None
        return {"kind": "bench", "issue": issues[0],
                "conclusion": "success" if payload["state"] == "success" else "failure"}
    return None


def issue_labels(gh, number):
    return {l["name"] for l in json.loads(
        gh(["api", f"repos/{{owner}}/{{repo}}/issues/{number}/labels"]))}


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_decide = sub.add_parser("decide", help="route a GitHub event")
    p_decide.add_argument("--event-name", required=True)
    p_decide.add_argument("--event-path", required=True)
    p_settle = sub.add_parser("settle", help="route an agent job's result")
    p_settle.add_argument("--issue", required=True, type=int)
    who = p_settle.add_mutually_exclusive_group(required=True)
    who.add_argument("--builder", choices=["done", "blocked", "protected", "failed"])
    who.add_argument("--inspector", choices=["approve", "changes"])
    for p in (p_decide, p_settle):
        p.add_argument("--owner", required=True)
        p.add_argument("--paused", action="store_true")
    args = parser.parse_args(argv)

    if args.command == "decide":
        payload = json.loads(pathlib.Path(args.event_path).read_text(encoding="utf-8"))
        event = facts(args.event_name, payload)
        if event is None:
            decision = _nothing(None, f"{args.event_name} event: not a pipeline event")
        else:
            if event["kind"] == "tick" and not args.paused:
                event["issue"], _ = cr_queue.pick(gh, args.owner)
            labels = issue_labels(gh, event["issue"]) if event.get("issue") else set()
            decision = route(event, labels, args.owner, args.paused)
    else:
        event = ({"kind": "builder", "issue": args.issue, "result": args.builder}
                 if args.builder else
                 {"kind": "inspector", "issue": args.issue, "verdict": args.inspector})
        decision = route(event, issue_labels(gh, args.issue), args.owner, args.paused)
    print(json.dumps(decision.as_dict()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
