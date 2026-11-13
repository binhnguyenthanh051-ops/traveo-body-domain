#!/usr/bin/env python3
"""release_check.py -- the release gate against the Definition of Done (build step 9).

    python host_tools/release_check.py --tag m6-diag --milestone M6 --sha <commit> --area REQ-DIAG [--manual-confirmed]

Run by .github/workflows/release.yml before it tags, and by the owner at any time to see how
close a milestone is. DoD rule 11 (docs/project/plan.md) is the source of truth; the section 11
items of the design note that are not in it yet are checked but report-only (.github/pipeline/
release.json), so there is one DoD, not two.

  traceability       trace_report --check --gate <area>: current, gap-free (DoD 1-6)   blocking
  release_notes      docs/releases/<tag>.md exists (DoD 7)                            blocking
  review_record      docs/review/M<n>-review.md: three sections, a verdict each (DoD 9) blocking
  manual             the owner ticked: post reviewed (DoD 8), no architecture drift     blocking
  foreign_release    no GitHub release that release.yml did not make (ADR-0026 sec. 3) blocking
  milestone_done     no open issue left in the milestone                               report-only
  bench_release      a `bench-release` success status on this commit                   report-only
  release_notes_crs  the notes name every CR closed as done in the milestone           report-only
  security           the security check (build step 12)                                not yet
Exit code: 0 when nothing blocking failed, 1 otherwise.
"""
import argparse
import dataclasses
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / ".github" / "pipeline" / "release.json"
API = "repos/{owner}/{repo}"
REVIEW_SECTIONS = ("Requirements", "Design", "Tests")


@dataclasses.dataclass
class Result:
    name: str
    state: str          # pass | fail | not yet
    detail: str
    blocking: bool = True


def load_config(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def blocking_failures(results):
    return [r for r in results if r.state == "fail" and r.blocking]


def _review_problems(text):
    problems = []
    for name in REVIEW_SECTIONS:
        m = re.search(r"^## \d+\. " + name + r"\b.*?(?=^## |\Z)", text, re.M | re.S)
        if not m:
            problems.append(f"no '{name}' section")
        elif "**Verdict:**" not in m.group(0):
            problems.append(f"'{name}' has no verdict")
    return problems


def _review_file(root, tag):
    m = re.match(r"m(\d+)([a-z]?)-", tag)
    if not m:
        return None
    names = [f"M{m.group(1)}{m.group(2).upper()}-review.md", f"M{m.group(1)}-review.md"]
    for name in names:
        path = root / "docs" / "review" / name
        if path.is_file():
            return path
    return None


def run_checks(*, tag, milestone, sha, root, gh, trace, manual_confirmed, report_only,
               known_releases):
    root = pathlib.Path(root)
    out = []

    rc = trace()
    out.append(Result("traceability", "pass" if rc == 0 else "fail",
                      "trace_report --check --gate: current and gap-free" if rc == 0
                      else f"trace_report exit {rc}: stale report or gaps in the area"))

    notes = root / "docs" / "releases" / f"{tag}.md"
    out.append(Result("release_notes", "pass" if notes.is_file() else "fail",
                      notes.relative_to(root).as_posix() + (" exists" if notes.is_file() else " missing")))

    review = _review_file(root, tag)
    if review is None:
        out.append(Result("review_record", "fail", "no docs/review/M<n>-review.md for this tag"))
    else:
        problems = _review_problems(review.read_text(encoding="utf-8"))
        out.append(Result("review_record", "fail" if problems else "pass",
                          "; ".join(problems) or f"{review.name}: three sections, a verdict each"))

    out.append(Result("manual", "pass" if manual_confirmed else "fail",
                      "owner ticked: post reviewed, no architecture-doc drift" if manual_confirmed
                      else "not ticked in the release form: post reviewed, no architecture drift"))

    releases = json.loads(gh(["api", f"{API}/releases?per_page=100"]))
    foreign = [r["tag_name"] for r in releases
               if (r.get("author") or {}).get("login") != "github-actions[bot]"
               and r["tag_name"] not in known_releases]
    out.append(Result("foreign_release", "fail" if foreign else "pass",
                      "releases not made by release.yml: " + ", ".join(foreign) if foreign
                      else "every release came from release.yml (or is listed as known)"))

    milestones = json.loads(gh(["api", f"{API}/milestones?state=all&per_page=100"]))
    number = next((m["number"] for m in milestones if m["title"] == milestone), None)
    if number is None:
        out.append(Result("milestone_done", "fail", f"no milestone '{milestone}'"))
        out.append(Result("release_notes_crs", "fail", f"no milestone '{milestone}'"))
    else:
        open_items = [i["number"] for i in json.loads(gh(
            ["api", f"{API}/issues?milestone={number}&state=open&per_page=100"]))
            if "pull_request" not in i]
        out.append(Result("milestone_done", "fail" if open_items else "pass",
                          "open: " + ", ".join(f"#{n}" for n in open_items) if open_items
                          else f"no open issue in {milestone}"))
        done = [i["number"] for i in json.loads(gh(
            ["api", f"{API}/issues?milestone={number}&state=closed&labels=done&per_page=100"]))
            if "pull_request" not in i]
        text = notes.read_text(encoding="utf-8") if notes.is_file() else ""
        missing = [n for n in done if not re.search(rf"#{n}\b", text)]
        out.append(Result("release_notes_crs", "fail" if missing else "pass",
                          "notes miss " + ", ".join(f"#{n}" for n in missing) if missing
                          else f"notes name all {len(done)} done CRs"))

    statuses = json.loads(gh(["api", f"{API}/commits/{sha}/statuses?per_page=100"]))
    latest = next((s for s in statuses if s["context"] == "bench-release"), None)
    state = latest["state"] if latest else "none"
    out.append(Result("bench_release", "pass" if state == "success" else "fail",
                      f"bench-release on {sha[:7]}: {state}"))

    out.append(Result("security", "not yet", "the security check arrives with build step 12",
                      blocking=False))

    for r in out:
        if r.name in report_only:
            r.blocking = False
    return out


def run_trace(root, area):
    return subprocess.run([sys.executable, str(pathlib.Path(root) / "host_tools" / "trace_report.py"),
                           "--check", "--gate", area], cwd=root).returncode


def run_gh(args, stdin=None):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin).stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True)
    parser.add_argument("--milestone", required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--area", required=True, help="requirement area, e.g. REQ-SECOC")
    parser.add_argument("--manual-confirmed", action="store_true")
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    args = parser.parse_args(argv)

    config = load_config(args.config)
    results = run_checks(
        tag=args.tag, milestone=args.milestone, sha=args.sha, root=args.root, gh=gh,
        trace=lambda: run_trace(args.root, args.area), manual_confirmed=args.manual_confirmed,
        report_only=set(config["report_only"]), known_releases=set(config["known_releases"]))

    print(f"Release check for {args.tag} ({args.milestone}, {args.sha[:7]})\n")
    print("| Check | State | Blocks | Detail |\n|---|---|---|---|")
    for r in results:
        print(f"| {r.name} | {r.state.upper()} | {'yes' if r.blocking else 'no'} | {r.detail} |")
    failed = blocking_failures(results)
    print("\n" + ("BLOCKED by: " + ", ".join(r.name for r in failed) if failed else "READY to tag"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
