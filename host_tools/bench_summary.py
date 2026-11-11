#!/usr/bin/env python3
"""bench_summary.py -- the numbers a bench record needs, from raw logview captures.

    python host_tools/bench_summary.py docs/bench/2026-11-05/nodeA.bin docs/bench/2026-11-05/nodeB.bin

Per capture: records, lost records (REQ-LOG-007), every LOG_EVT_BOOT with the
image whose banner preceded it and how many banners it had (F-006), the reset
cause decoded (TRM Table 19-2; the FBL's is a mapped enum, F-016), FBL jumps,
SecOC accepts per CAN ID, rejects per reason + CAN ID, and each task's minimum
free stack words (LOG_EVT_TASK_STACK, ADR-0025 D5).

    python host_tools/bench_summary.py nodeA.bin --stack-report nodeA.bin=stack_bound_nodeA_app.json

With --stack-report, each task's measured stack use (configured - free words)
is checked against stack_bound's static worst case from that JSON report: the
static number must never be smaller. Exit 1 if it is (the analysis is wrong).

It reads the same stream logview does, through lib/log_decode, so a bench record
never rests on a grep of a rendered transcript.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib import log_decode  # noqa: E402

EVT = log_decode.log_events.NAME_TO_ID
BOOT, JUMP, ACCEPT = EVT["LOG_EVT_BOOT"], EVT["LOG_EVT_APP_JUMP"], EVT["LOG_EVT_SECOC_ACCEPT"]
REJECTS = {EVT[n]: n for n in ("LOG_EVT_SECOC_REJECT_MAC", "LOG_EVT_SECOC_REJECT_FRESHNESS")}
TASK_STACK = EVT["LOG_EVT_TASK_STACK"]

# LOG_EVT_TASK_STACK task_id (events.csv) -> (short name, stack_bound task entry)
TASKS = {1: ("can", "can_task"), 2: ("app", "app_task"), 3: ("health", "health_task"),
         4: ("log", "log_cyclic_task"), 5: ("heartbeat", "heartbeat_task"),
         6: ("idle", "prvIdleTask"), 7: ("timer", "prvTimerTask")}
WORD = 4

BANNER = re.compile(r"--- (\S+?)(?:/sec)? \S+ ---")

# RES_CAUSE bits, TRM Table 19-2 (body controller entry TRM, §19.2), high bit first.
RESET_BITS = [
    (30, "PORVDDD"), (26, "PMIC"), (25, "OCD_REGHC"), (24, "OCD_DPSLP_LINREG"),
    (23, "OCD_ACT_LINREG"), (22, "OVDVCCD"), (21, "OVDVDDA"), (20, "OVDVDDD"),
    (19, "BODVCCD"), (18, "BODVDDA"), (17, "BODVDDD"), (16, "XRES"),
    (8, "MCWDT3"), (7, "MCWDT2"), (6, "MCWDT1"), (5, "MCWDT0"), (4, "SOFT"),
    (3, "TC_DBGRESET"), (2, "DPSLP_FAULT"), (1, "ACT_FAULT"), (0, "WDT"),
]

# fbl_reset_cause_t (shared/boot/include/boot_types.h): what the FBL logs instead (F-016).
FBL_CAUSES = ["FBL_RST_POWER_ON", "FBL_RST_SOFTWARE", "FBL_RST_WATCHDOG",
              "FBL_RST_DEBUG", "FBL_RST_HIBERNATE", "FBL_RST_OTHER"]


def reset_bits(value: int) -> list[str]:
    return [name for bit, name in RESET_BITS if value & (1 << bit)]


def describe_reset(image: str, value: int) -> str:
    if image == "fbl":
        name = FBL_CAUSES[value] if value < len(FBL_CAUSES) else f"0x{value:X}"
        return f"{name} (mapped enum, F-016)"
    return " | ".join(reset_bits(value)) or "none"


@dataclass
class Boot:
    image: str          # from the banner just before it; "?" if none was seen
    banners: int        # banners since the previous boot (F-006: should be 1)
    reset_reason: int


@dataclass
class Summary:
    records: int = 0
    lost: int = 0
    resyncs: int = 0
    jumps: int = 0
    boots: list[Boot] = field(default_factory=list)
    accepts: Counter = field(default_factory=Counter)
    rejects: Counter = field(default_factory=Counter)
    min_free: dict[int, int] = field(default_factory=dict)     # task_id -> min free words seen

    @property
    def max_banners_per_boot(self) -> int:
        return max((b.banners for b in self.boots), default=0)


def summarize(data: bytes) -> Summary:
    dec = log_decode.Decoder()
    items = dec.feed(data) + dec.flush()
    s = Summary()
    text, image, pending = "", "?", 0
    for item in items:
        if isinstance(item, log_decode.Text):
            text += item.text
            matches = list(BANNER.finditer(text))
            if matches:
                pending += len(matches)
                image = matches[-1].group(1)
                text = text[matches[-1].end():]
            text = text[-128:]                      # a banner split across chunks
        elif isinstance(item, log_decode.Record):
            if item.evt == BOOT:
                s.boots.append(Boot(image, pending, item.arg0))
                pending = 0
            elif item.evt == JUMP:
                s.jumps += 1
            elif item.evt == ACCEPT:
                s.accepts[item.arg0] += 1
            elif item.evt in REJECTS:
                s.rejects[(REJECTS[item.evt], item.arg0)] += 1
            elif item.evt == TASK_STACK:
                s.min_free[item.arg0] = min(item.arg1, s.min_free.get(item.arg0, item.arg1))
    s.records, s.lost, s.resyncs = dec.records, dec.lost, dec.resyncs
    return s


def format_summary(label: str, s: Summary) -> str:
    out = [f"== {label}",
           f"records: {s.records}   lost: {s.lost}   resyncs: {s.resyncs}",
           f"boots: {len(s.boots)}   max banners per boot: {s.max_banners_per_boot}   jumps: {s.jumps}"]
    for i, b in enumerate(s.boots, 1):
        out.append(f"  boot {i:>2}  {b.image:<9} banners={b.banners}  "
                   f"reset_reason=0x{b.reset_reason:X}  ({describe_reset(b.image, b.reset_reason)})")
    out.append("accepts: " + (", ".join(f"0x{k:03X} x{v}" for k, v in sorted(s.accepts.items()))
                              or "none"))
    out.append(f"rejects: {sum(s.rejects.values())}")
    for (name, can_id), n in sorted(s.rejects.items()):
        out.append(f"  {name} can_id=0x{can_id:03X} x{n}")
    out.append("task stacks, min free words: " + (", ".join(
        f"{TASKS.get(t, (f'id{t}',))[0]} {w}" for t, w in sorted(s.min_free.items())) or "none"))
    return "\n".join(out)


def check_stacks(s: Summary, report: dict) -> tuple[bool, list[str]]:
    """Measured use (configured - min free) against stack_bound's static worst case, per task.

    The static bound must be >= what the bench measured, or the analysis missed a path.
    A task in the report with no LOG_EVT_TASK_STACK in the capture is a failure too: no
    evidence is not a pass."""
    by_entry = {entry: tid for tid, (_, entry) in TASKS.items()}
    ok, out = True, [f"stack check against {report['image']['name']} "
                     f"(sha256 {report['image']['sha256'][:12]}, {report['tool']['name']} "
                     f"{report['tool']['version']}):"]
    for st in report["stacks"]:
        tid = by_entry.get(st["entry"])
        if tid is None or tid not in s.min_free:
            ok = False
            out.append(f"  {st['entry']:<16} NO DATA: no LOG_EVT_TASK_STACK for it in the capture")
            continue
        used = st["configured"] - WORD * s.min_free[tid]
        good = used <= st["required"]
        ok &= good
        out.append(f"  {st['entry']:<16} measured {used:>5} B <= static {st['required']:>5} B "
                   f"of {st['configured']:>5} B  {'ok' if good else 'VIOLATION: the static bound is too small'}")
    for tid in sorted(set(s.min_free) - {by_entry.get(st["entry"]) for st in report["stacks"]}):
        name = TASKS.get(tid, (f"id{tid}",))[0]
        out.append(f"  {name:<16} {s.min_free[tid]} words free, no static bound (not in the report)")
    return ok, out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+", type=Path, help="raw logview captures (--raw .bin)")
    ap.add_argument("--stack-report", action="append", default=[], metavar="CAPTURE=JSON",
                    help="check that capture's task stacks against a stack_bound JSON report")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")     # as logview (F-005)
    reports = {}
    for spec in args.stack_report:
        cap, sep, rep = spec.partition("=")
        if not sep:
            ap.error(f"--stack-report wants CAPTURE=JSON, got {spec!r}")
        reports[Path(cap).name] = Path(rep)
    unknown = set(reports) - {p.name for p in args.captures}
    if unknown:
        ap.error(f"--stack-report names captures not given: {', '.join(sorted(unknown))}")
    rc = 0
    for path in args.captures:
        s = summarize(path.read_bytes())
        print(format_summary(path.name, s))
        if path.name in reports:
            ok, lines = check_stacks(s, json.loads(reports[path.name].read_text(encoding="utf-8")))
            print("\n".join(lines))
            rc |= 0 if ok else 1
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
