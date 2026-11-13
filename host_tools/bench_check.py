#!/usr/bin/env python3
"""bench_check.py -- the bench verdict for one CR, from raw captures, by exit code.

    python host_tools/bench_check.py --node A=nodeA.bin --node B=nodeB.bin [--expect bench/expect.json]

Always, per capture: records were decoded, none was lost (REQ-LOG-007), the node booted
(LOG_EVT_BOOT), and each boot printed one banner (F-006). On Node A the FBL jumped to the app.

Per CR, `--expect` (the PR's bench/expect.json, written by the Builder from the acceptance
criteria and checked by the Inspector) names contract events:

    {"A": {"at_least": {"LOG_EVT_APP_JUMP": 1}}, "B": {"none": ["LOG_EVT_SECOC_REJECT_MAC"]}}

Diagnostic events are never evidence (REQ-LOG-008), so naming one is refused.
Exit code: 0 pass, 1 fail, 2 the input is unusable (missing capture, invalid expect file).
"""
import argparse
import json
import pathlib
import sys
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from lib import log_decode, log_events  # noqa: E402
import bench_summary                    # noqa: E402

NODES = ("A", "B")
RULES = ("at_least", "none")


def expect_problems(expect):
    problems = []
    for node, rules in expect.items():
        if node not in NODES:
            problems.append(f"unknown node '{node}' (use A or B)")
            continue
        for rule, value in rules.items():
            if rule not in RULES:
                problems.append(f"{node}: unknown rule '{rule}' (use at_least or none)")
                continue
            names = value if rule == "none" else list(value)
            if rule == "at_least" and any(not isinstance(n, int) or n < 1 for n in value.values()):
                problems.append(f"{node}: at_least counts must be whole numbers >= 1")
            for name in names:
                evt = log_events.NAME_TO_ID.get(name)
                if evt is None:
                    problems.append(f"{node}: unknown event '{name}'")
                elif not log_events.is_contract(evt):
                    problems.append(f"{node}: '{name}' is a diagnostic event, never evidence (REQ-LOG-008)")
    return problems


def check(node, data, rules):
    """Return the list of failures for one capture."""
    s = bench_summary.summarize(data)
    counts = Counter(log_events.name(item.evt) for item in log_decode.decode_all(data)
                     if isinstance(item, log_decode.Record))
    failures = []
    if s.records == 0:
        failures.append("no records decoded")
    if s.lost:
        failures.append(f"{s.lost} records lost (REQ-LOG-007)")
    if not s.boots:
        failures.append("no boot (LOG_EVT_BOOT) seen")
    if s.max_banners_per_boot > 1:
        failures.append(f"{s.max_banners_per_boot} banners in one boot (F-006)")
    if node == "A" and s.boots and s.jumps == 0:
        failures.append("the FBL did not jump to the app (LOG_EVT_APP_JUMP)")
    for name, at_least in rules.get("at_least", {}).items():
        if counts[name] < at_least:
            failures.append(f"{name}: expected at least {at_least}, saw {counts[name]}")
    for name in rules.get("none", []):
        if counts[name]:
            failures.append(f"{name}: expected none, saw {counts[name]}")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--node", action="append", required=True, metavar="A=PATH",
                        help="a node and its raw capture; give A and B")
    parser.add_argument("--expect", help="the PR's bench/expect.json")
    args = parser.parse_args(argv)

    captures = dict(n.split("=", 1) for n in args.node)
    expect = {}
    if args.expect:
        try:
            expect = json.loads(pathlib.Path(args.expect).read_text(encoding="utf-8"))
        except (OSError, ValueError) as err:
            print(f"INVALID: cannot read {args.expect}: {err}")
            return 2
        problems = expect_problems(expect)
        if problems:
            print("INVALID expect file:\n" + "\n".join(f"- {p}" for p in problems))
            return 2

    failed = False
    for node in NODES:
        path = captures.get(node)
        if path is None or not pathlib.Path(path).is_file():
            print(f"INVALID: no capture for node {node}")
            return 2
        failures = check(node, pathlib.Path(path).read_bytes(), expect.get(node, {}))
        print(f"Node {node}: " + ("ok" if not failures else "FAIL"))
        for f in failures:
            print(f"  - {f}")
        failed = failed or bool(failures)
    print("FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
