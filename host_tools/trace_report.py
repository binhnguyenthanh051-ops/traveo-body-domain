#!/usr/bin/env python3
"""trace_report.py -- generate docs/traceability.md and fail on traceability gaps.

DoD rule 11 (docs/project/plan.md) and docs/traceability-convention.md: every
REQ-* traces requirement -> ADR -> impl tag -> test tag -> silicon evidence.
This script checks those links mechanically, so a script (not memory) catches a
gap. It is deterministic on purpose: no judgment, no LLM, an exact right answer.

Inputs (read only):
    docs/requirements/*.md            REQ headings ("### REQ-X-NNN — title") and the
                                      coverage matrix (Traces / Verified by / On silicon)
    docs/architecture/decisions/*.md  ADR ids, status, sub-decision headings ("### Dn.")
    source files (git-tracked + untracked-not-ignored; .c .h .py .s .S)
                                      the impl / test / design-only tags (convention §2)

Output: docs/traceability.md (generated, committed as an artifact).

A GAP (exit 1) is any of:
    no-impl         a REQ with no impl tag and no design-only tag
    no-test         a REQ with no test tag and no design-only tag
    dangling        a tag naming a REQ, ADR or ADR sub-decision that does not exist
    malformed       a tag whose ID looks like a REQ/ADR but doesn't match the scheme
    bad-trace       a requirement tracing to an ADR / sub-decision that does not exist
    matrix-drift    the matrix says test_x verifies a REQ, but test_x has no test tag for it
    matrix-only / body-only   a REQ in the coverage matrix but not the body, or vice versa
    no-silicon      an "On silicon" cell that is empty, names a missing file, or says
                    n/a / deferred without a reason
    no-silicon-col  a requirements file with no "On silicon" column at all

Reported but NOT a gap: accepted ADRs without a test tag (backfilling M1-M4 is
opportunistic, convention §6).

On silicon cell values:
    `docs/bench/<date>/<file>`  ...   evidence; every backticked path must exist
    n/a — <reason>                    not target behaviour (e.g. a host-side contract)
    deferred — <reason>               explicitly deferred; listed in the report
    (empty), —, ⏳ ...                no evidence yet => gap

Usage:
    python host_tools/trace_report.py                   # write the report; exit 1 on gaps
    python host_tools/trace_report.py --check           # don't write; exit 1 if stale or gaps
    python host_tools/trace_report.py --gate REQ-SECOC  # only gaps on REQ-SECOC-* fail the run
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REQ_DIR = Path("docs") / "requirements"
ADR_DIR = Path("docs") / "architecture" / "decisions"
OUT_PATH = Path("docs") / "traceability.md"

SOURCE_SUFFIXES = {".c", ".h", ".py", ".s", ".S"}
# docs/ mentions tags in prose; the rest is build output or third-party code.
SKIP_DIRS = {".git", "build", "vendor", "docs", "libs", "mtb_shared",
             "__pycache__", ".venv", "venv", "node_modules"}

REQ_ID_RE = re.compile(r"REQ-[A-Z][A-Z0-9]*-\d{3}")
ADR_ID_RE = re.compile(r"ADR-\d{4}")
# keyword, ID token, optional " Dn" sub-decision (ADR tags only).
TAG_RE = re.compile(r"@(impl|test|design-only)[ \t]+([^\s:*]+)(?:[ \t]+D(\d+)\b)?")
REQ_HEADING_RE = re.compile(rf"^###\s+({REQ_ID_RE.pattern})\b\s*[—–-]*\s*(.*)$")
ADR_REF_RE = re.compile(r"(ADR-\d{4})((?:[ ,]+D\d+\b)*)")
SUBDECISION_RE = re.compile(r"^#{2,4}\s+D(\d+)\b", re.MULTILINE)
STATUS_RE = re.compile(r"\*\*Status:\*\*\s*([A-Za-z-]+)")

EMPTY_CELLS = {"", "—", "-", "–", "todo", "tbd"}
KINDS = ("impl", "test", "design-only")


@dataclass(frozen=True)
class Tag:
    kind: str           # impl | test | design-only
    id: str             # REQ-SECOC-001 or ADR-0021
    sub: int | None     # sub-decision number for ADR tags
    path: str           # repo-relative, posix
    line: int


@dataclass
class Adr:
    id: str
    title: str
    status: str
    path: str
    subs: set[int]


@dataclass
class Req:
    id: str
    source: str                       # requirements file, repo-relative
    title: str = ""
    short: str = ""
    in_body: bool = False
    in_matrix: bool = False
    traces: list[tuple[str, int | None]] = field(default_factory=list)
    verified_by: list[str] = field(default_factory=list)
    silicon: str | None = None        # raw cell; None = the file has no such column


@dataclass(frozen=True)
class Gap:
    kind: str
    id: str
    detail: str
    where: str = ""


# ---------------------------------------------------------------- parsing --

def _cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def _add_trace(req: Req, adr: str, sub: int | None) -> None:
    if (adr, sub) not in req.traces:
        req.traces.append((adr, sub))


def parse_requirements(path: Path, rel: str) -> tuple[list[Req], bool]:
    """Return (requirements in file order, has an "On silicon" column)."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    primary = ADR_ID_RE.search(text)
    primary_adr = primary.group(0) if primary else None
    reqs: dict[str, Req] = {}

    def get(req_id: str) -> Req:
        return reqs.setdefault(req_id, Req(id=req_id, source=rel))

    # Coverage matrix: the first table whose header has an "ID" column.
    has_silicon_col = False
    col: dict[str, int] = {}
    in_table = False
    for line in lines:
        if not line.lstrip().startswith("|"):
            if in_table:
                break
            continue
        cells = _cells(line)
        if not in_table:
            names = [c.lower() for c in cells]
            if "id" in names:
                in_table = True
                for i, name in enumerate(names):
                    if name == "id":
                        col["id"] = i
                    elif name.startswith("requirement"):
                        col["short"] = i
                    elif name.startswith("traces"):
                        col["traces"] = i
                    elif name.startswith("verified"):
                        col["verified"] = i
                    elif name.startswith("on silicon"):
                        col["silicon"] = i
                has_silicon_col = "silicon" in col
            continue
        if set("".join(cells)) <= set("-: "):
            continue                                    # the |---|---| separator
        m = REQ_ID_RE.fullmatch(cells[col["id"]]) if col["id"] < len(cells) else None
        if not m:
            continue
        req = get(m.group(0))
        req.in_matrix = True

        def cell(key: str) -> str:
            i = col.get(key)
            return cells[i] if i is not None and i < len(cells) else ""

        req.short = cell("short")
        traces = cell("traces")
        for adr, subs in ADR_REF_RE.findall(traces):
            _add_trace(req, adr, None)
        bare = ADR_REF_RE.sub("", traces)          # "D1, D9" => sub-decisions of the primary ADR
        if primary_adr:
            for n in re.findall(r"\bD(\d+)\b", bare):
                _add_trace(req, primary_adr, int(n))
        req.verified_by = [t for t in re.findall(r"`(test_\w+)`", cell("verified"))]
        if has_silicon_col:
            req.silicon = cell("silicon")

    # Body: "### REQ-X-NNN — title" up to the next heading.
    current: Req | None = None
    for line in lines:
        m = REQ_HEADING_RE.match(line)
        if m:
            current = get(m.group(1))
            current.in_body = True
            current.title = m.group(2).strip()
            continue
        if line.startswith("#"):
            current = None
            continue
        if current is not None:
            for adr, subs in ADR_REF_RE.findall(line):
                nums = re.findall(r"D(\d+)", subs)
                if nums:
                    for n in nums:
                        _add_trace(current, adr, int(n))
                elif not any(a == adr for a, _ in current.traces):
                    _add_trace(current, adr, None)

    return list(reqs.values()), has_silicon_col


def parse_adrs(root: Path) -> dict[str, Adr]:
    adrs: dict[str, Adr] = {}
    adr_dir = root / ADR_DIR
    if not adr_dir.is_dir():
        return adrs
    for path in sorted(adr_dir.glob("ADR-*.md")):
        m = ADR_ID_RE.match(path.name)
        if not m:
            continue
        text = path.read_text(encoding="utf-8")
        title = next((ln[2:].strip() for ln in text.splitlines() if ln.startswith("# ")), "")
        status = STATUS_RE.search(text)
        adrs[m.group(0)] = Adr(
            id=m.group(0),
            title=title.split(":", 1)[-1].strip() if ":" in title else title,
            status=status.group(1).lower() if status else "unknown",
            path=path.relative_to(root).as_posix(),
            subs={int(n) for n in SUBDECISION_RE.findall(text)},
        )
    return adrs


def source_files(root: Path) -> list[str]:
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
             "--exclude-standard"],
            capture_output=True, check=True).stdout
        rels = {p for p in out.decode("utf-8").split("\0") if p}
    except (OSError, subprocess.CalledProcessError):
        rels = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    keep = []
    for rel in sorted(rels):
        parts = rel.split("/")
        if any(d in SKIP_DIRS for d in parts[:-1]):
            continue
        if Path(rel).suffix not in SOURCE_SUFFIXES or not (root / rel).is_file():
            continue
        keep.append(rel)
    return keep


def scan_tags(root: Path, files: list[str]) -> tuple[list[Tag], list[Gap]]:
    tags: list[Tag] = []
    gaps: list[Gap] = []
    for rel in files:
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in TAG_RE.finditer(line):
                kind, token, sub = m.group(1), m.group(2), m.group(3)
                where = f"{rel}:{lineno}"
                if REQ_ID_RE.fullmatch(token):
                    tags.append(Tag(kind, token, None, rel, lineno))
                elif ADR_ID_RE.fullmatch(token):
                    tags.append(Tag(kind, token, int(sub) if sub else None, rel, lineno))
                elif token.startswith(("REQ", "ADR")):
                    gaps.append(Gap("malformed", token,
                                    f"tag ID `{token}` doesn't match the scheme", where))
    return tags, gaps


# --------------------------------------------------------------- analysis --

@dataclass
class Model:
    reqs: list[Req]
    adrs: dict[str, Adr]
    tags: list[Tag]
    gaps: list[Gap]
    silicon_cols: dict[str, bool]          # requirements file -> has the column

    def tags_for(self, req_id: str, kind: str) -> list[Tag]:
        return [t for t in self.tags if t.id == req_id and t.kind == kind]


def silicon_problem(cell: str, root: Path) -> str | None:
    c = cell.strip()
    low = c.lower()
    if low in EMPTY_CELLS or low.startswith("⏳"):
        return "no silicon evidence yet"
    for kw in ("n/a", "deferred"):
        if low.startswith(kw):
            return None if c[len(kw):].strip(" :—–-") else f"`{kw}` without a reason"
    paths = re.findall(r"`([^`]+)`", c)
    if not paths:
        return "names no evidence path (put the capture path in backticks)"
    missing = [p for p in paths if not (root / p.split("#")[0]).exists()]
    if missing:
        return "evidence path not found: " + ", ".join(f"`{p}`" for p in missing)
    return None


def build_model(root: Path) -> Model:
    reqs: list[Req] = []
    silicon_cols: dict[str, bool] = {}
    req_dir = root / REQ_DIR
    for path in sorted(req_dir.glob("*.md")) if req_dir.is_dir() else []:
        rel = path.relative_to(root).as_posix()
        file_reqs, has_col = parse_requirements(path, rel)
        if file_reqs:
            reqs.extend(file_reqs)
            silicon_cols[rel] = has_col
    reqs.sort(key=lambda r: r.id)
    adrs = parse_adrs(root)
    files = source_files(root)
    tags, gaps = scan_tags(root, files)
    stems = {Path(f).stem for f in files}
    known = {r.id for r in reqs}

    for t in tags:
        where = f"{t.path}:{t.line}"
        if t.id.startswith("REQ-"):
            if t.id not in known:
                gaps.append(Gap("dangling", t.id, f"tag names an undefined requirement", where))
        elif t.id not in adrs:
            gaps.append(Gap("dangling", t.id, "tag names an ADR that doesn't exist", where))
        elif t.sub is not None and t.sub not in adrs[t.id].subs:
            gaps.append(Gap("dangling", f"{t.id} D{t.sub}",
                            f"{t.id} has no sub-decision D{t.sub}", where))

    model = Model(reqs, adrs, tags, gaps, silicon_cols)
    for rel, has_col in silicon_cols.items():
        if not has_col:
            area = next(_area(r.id) for r in reqs if r.source == rel)
            gaps.append(Gap("no-silicon-col", area,
                            "requirements file has no \"On silicon\" column", rel))

    for r in reqs:
        has_matrix = any(x.in_matrix for x in reqs if x.source == r.source)
        if r.in_matrix and not r.in_body:
            gaps.append(Gap("matrix-only", r.id, "in the coverage matrix but has no body section",
                            r.source))
        if r.in_body and has_matrix and not r.in_matrix:
            gaps.append(Gap("body-only", r.id, "has a body section but no coverage-matrix row",
                            r.source))
        for adr, sub in r.traces:
            if adr not in adrs:
                gaps.append(Gap("bad-trace", r.id, f"traces to {adr}, which doesn't exist",
                                r.source))
            elif sub is not None and sub not in adrs[adr].subs:
                gaps.append(Gap("bad-trace", r.id, f"traces to {adr} D{sub}, which doesn't exist",
                                r.source))
        design = model.tags_for(r.id, "design-only")
        if not model.tags_for(r.id, "impl") and not design:
            gaps.append(Gap("no-impl", r.id, "no impl tag (and no design-only tag)"))
        test_tags = model.tags_for(r.id, "test")
        if not test_tags and not design:
            gaps.append(Gap("no-test", r.id, "no test tag (and no design-only tag)"))
        tested_in = {Path(t.path).stem for t in test_tags}
        for name in r.verified_by:
            if name not in stems:
                gaps.append(Gap("matrix-drift", r.id,
                                f"matrix names `{name}`, but no such test file exists", r.source))
            elif name not in tested_in:
                gaps.append(Gap("matrix-drift", r.id,
                                f"matrix names `{name}`, but it has no test tag for {r.id}",
                                r.source))
        if r.silicon is not None:
            problem = silicon_problem(r.silicon, root)
            if problem:
                gaps.append(Gap("no-silicon", r.id, problem, r.source))
    return model


def _area(req_id: str) -> str:
    return req_id.rsplit("-", 1)[0]


# -------------------------------------------------------------- rendering --

def _fmt_traces(traces: list[tuple[str, int | None]]) -> str:
    grouped: dict[str, list[int]] = {}
    for adr, sub in traces:
        grouped.setdefault(adr, [])
        if sub is not None and sub not in grouped[adr]:
            grouped[adr].append(sub)
    return "; ".join(f"{a} " + ", ".join(f"D{n}" for n in subs) if subs else a
                     for a, subs in grouped.items())


def _fmt_files(tags: list[Tag], prefix: str = "") -> list[str]:
    counts: dict[str, int] = {}
    for t in tags:
        counts[t.path] = counts.get(t.path, 0) + 1
    return [f"{prefix}`{p}`" + (f" ×{n}" if n > 1 else "") for p, n in sorted(counts.items())]


def _md(text: str) -> str:
    return text.replace("|", "\\|")


def render(model: Model) -> str:
    gaps_by_req: dict[str, list[Gap]] = {}
    for g in model.gaps:
        gaps_by_req.setdefault(g.id, []).append(g)
    areas: dict[str, list[Req]] = {}
    for r in model.reqs:
        areas.setdefault(_area(r.id), []).append(r)

    out: list[str] = [
        "# Traceability report",
        "",
        "> **Generated** by `host_tools/trace_report.py` — do not edit by hand. Regenerate with",
        "> `python host_tools/trace_report.py`; `--check` fails if this file is stale.",
        "> Tag scheme: `docs/traceability-convention.md`. Definition of Done: `docs/project/plan.md`",
        "> rule 11.",
        "",
        "## Summary",
        "",
        "| Area | Source | REQs | Fully traced | With gaps |",
        "|---|---|---|---|---|",
    ]
    for area, reqs in areas.items():
        bad = sum(1 for r in reqs if r.id in gaps_by_req)
        out.append(f"| {area} | `{reqs[0].source}` | {len(reqs)} | {len(reqs) - bad} | {bad} |")
    out += ["", f"**{len(model.gaps)} gap(s)** in total — see [Gaps](#gaps).", ""]

    for area, reqs in areas.items():
        out += [f"## {area}", "",
                "| REQ | Requirement | Traces | Impl | Test | On silicon | Status |",
                "|---|---|---|---|---|---|---|"]
        for r in reqs:
            impl = _fmt_files(model.tags_for(r.id, "impl"))
            impl += _fmt_files(model.tags_for(r.id, "design-only"), "design-only: ")
            test = _fmt_files(model.tags_for(r.id, "test"))
            silicon = "*(no column)*" if r.silicon is None else _md(r.silicon or "—")
            kinds = sorted({g.kind for g in gaps_by_req.get(r.id, [])})
            status = "✅" if not kinds else "❌ " + ", ".join(kinds)
            out.append(" | ".join([
                f"| {r.id}", _md(r.short or r.title), _fmt_traces(r.traces) or "—",
                "<br>".join(impl) or "—", "<br>".join(test) or "—", silicon, f"{status} |"]))
        out.append("")

    out += ["## Gaps", ""]
    if not model.gaps:
        out.append("None.")
    for g in sorted(model.gaps, key=lambda g: (g.id, g.kind, g.where, g.detail)):
        where = f" — `{g.where}`" if g.where else ""
        out.append(f"- **{g.id}** · `{g.kind}` · {_md(g.detail)}{where}")
    out.append("")

    deferred = [r for r in model.reqs
                if r.silicon and r.silicon.strip().lower().startswith("deferred")]
    out += ["## Deferred on silicon", ""]
    out += [f"- **{r.id}** — {_md(r.silicon or '')}" for r in deferred] or ["None."]
    out.append("")

    out += ["## ADR coverage (informational, not gated)", "",
            "Accepted ADRs without a test tag are listed, not failed: M1–M4 tags are backfilled",
            "opportunistically (`docs/traceability-convention.md` §6).", "",
            "| ADR | Title | Status | Impl tags | Test tags | Note |",
            "|---|---|---|---|---|---|"]
    for a in model.adrs.values():
        n_impl = sum(1 for t in model.tags if t.id == a.id and t.kind == "impl")
        n_test = sum(1 for t in model.tags if t.id == a.id and t.kind == "test")
        note = "no test tag" if a.status == "accepted" and n_test == 0 else ""
        out.append(f"| {a.id} | {_md(a.title)} | {a.status} | {n_impl} | {n_test} | {note} |")
    out.append("")
    return "\n".join(out)


# ------------------------------------------------------------------- main --

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT, help="repo root (default: this repo)")
    ap.add_argument("--out", type=Path, default=None, help=f"report path (default: {OUT_PATH})")
    ap.add_argument("--check", action="store_true",
                    help="don't write; fail if the committed report is stale")
    ap.add_argument("--gate", action="append", default=[], metavar="PREFIX",
                    help="only gaps whose ID starts with PREFIX fail the run (repeatable)")
    args = ap.parse_args(argv)

    root = args.root.resolve()
    out_path = args.out if args.out is not None else root / OUT_PATH
    model = build_model(root)
    report = render(model)

    rc = 0
    if args.check:
        current = out_path.read_text(encoding="utf-8") if out_path.exists() else None
        if current != report:
            print(f"STALE: {out_path} — run python host_tools/trace_report.py")
            rc = 1
    else:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(report, encoding="utf-8", newline="\n")
        print(f"wrote {out_path}")

    gating = [g for g in model.gaps
              if not args.gate or any(g.id.startswith(p) for p in args.gate)]
    for g in sorted(model.gaps, key=lambda g: (g.id, g.kind, g.where)):
        mark = "GAP " if g in gating else "info"
        where = f" ({g.where})" if g.where else ""
        print(f"{mark} [{g.kind}] {g.id}: {g.detail}{where}")
    print(f"{len(model.reqs)} requirements, {len(model.tags)} tags, "
          f"{len(model.gaps)} gap(s), {len(gating)} gating")
    if gating:
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
