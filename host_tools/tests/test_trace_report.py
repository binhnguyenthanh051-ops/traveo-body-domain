"""Tests for trace_report -- the DoD rule 11 traceability gate.

Each test builds a tiny repo in tmp_path (requirements file, ADR, tagged sources)
and asserts on the gaps the tool finds. Tag strings are assembled from AT so the
real repo scan doesn't pick up this file's fixtures as tags."""
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import trace_report  # noqa: E402

AT = "@"

ADR = """# ADR-0042: Demo decision

**Status:** accepted · **Date:** 2026-10-19

## Decisions

### D1. First

### D2. Second
"""

REQS_HEADER = """# Demo requirements

Each traces up to an ADR-0042 sub-decision.

## Coverage matrix

"""


OBSERVED_OK = "structural — fixture"     # always valid; needs no event registry


def matrix(rows, silicon=True, observed=True):
    """rows: (id, traces, verified, silicon[, observed])."""
    head = "| ID | Requirement (short) | Traces | Verified by |"
    sep = "|---|---|---|---|"
    if silicon:
        head += " On silicon |"
        sep += "---|"
    if observed:
        head += " Observed by |"
        sep += "---|"
    lines = [head, sep]
    for row in rows:
        req_id, traces, verified, sil = row[:4]
        obs = row[4] if len(row) > 4 else OBSERVED_OK
        line = f"| {req_id} | short {req_id} | {traces} | {verified} |"
        if silicon:
            line += f" {sil} |"
        if observed:
            line += f" {obs} |"
        lines.append(line)
    return "\n".join(lines) + "\n"


def body(*req_ids):
    return "\n## Requirements\n\n" + "".join(
        f"### {r} — title\nThe thing shall work. *(ADR-0042 D1.)*\n\n" for r in req_ids)


def make_repo(tmp_path, reqs_md, sources, adr=ADR, extra=None):
    (tmp_path / "docs" / "requirements").mkdir(parents=True)
    (tmp_path / "docs" / "architecture" / "decisions").mkdir(parents=True)
    (tmp_path / "docs" / "requirements" / "demo.md").write_text(reqs_md, encoding="utf-8")
    (tmp_path / "docs" / "architecture" / "decisions" / "ADR-0042-demo.md").write_text(
        adr, encoding="utf-8")
    for rel, text in {**sources, **(extra or {})}.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return tmp_path


def tag(kind, id_, hint="hint"):
    return f"/* {AT}{kind} {id_} : {hint} */\n"


CAPTURE = "docs/bench/2026-10-24/capture.txt"
GOOD_SOURCES = {
    "mod/src/demo.c": tag("impl", "REQ-DEMO-001") + tag("impl", "ADR-0042 D1"),
    "mod/tests/test_demo.c": tag("test", "REQ-DEMO-001") + tag("test", "ADR-0042 D2"),
    CAPTURE: "capture\n",
}


def good_repo(tmp_path, silicon=f"`{CAPTURE}`"):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "`test_demo`", silicon)]) + \
        body("REQ-DEMO-001")
    return make_repo(tmp_path, md, GOOD_SOURCES)


def kinds(model):
    return sorted((g.kind, g.id) for g in model.gaps)


def test_fully_traced_requirement_has_no_gaps(tmp_path):
    model = trace_report.build_model(good_repo(tmp_path))
    assert model.gaps == []
    report = trace_report.render(model)
    assert "| REQ-DEMO-001 |" in report
    assert "✅" in report
    assert "`mod/src/demo.c`" in report and "`mod/tests/test_demo.c`" in report


def test_missing_test_tag_is_a_gap(tmp_path):
    root = good_repo(tmp_path)
    (root / "mod/tests/test_demo.c").write_text("/* no tags */\n", encoding="utf-8")
    assert ("no-test", "REQ-DEMO-001") in kinds(trace_report.build_model(root))


def test_missing_impl_tag_is_a_gap(tmp_path):
    root = good_repo(tmp_path)
    (root / "mod/src/demo.c").write_text("/* no tags */\n", encoding="utf-8")
    assert ("no-impl", "REQ-DEMO-001") in kinds(trace_report.build_model(root))


def test_design_only_satisfies_impl_and_test(tmp_path):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "", "n/a — design-only residency")]) + \
        body("REQ-DEMO-001")
    root = make_repo(tmp_path, md, {"mod/src/demo.c": tag("design-only", "REQ-DEMO-001")})
    model = trace_report.build_model(root)
    assert model.gaps == []
    assert "design-only: `mod/src/demo.c`" in trace_report.render(model)


@pytest.mark.parametrize("bad_tag, gap", [
    (tag("impl", "REQ-DEMO-999"), ("dangling", "REQ-DEMO-999")),
    (tag("impl", "ADR-0099"), ("dangling", "ADR-0099")),
    (tag("test", "ADR-0042 D7"), ("dangling", "ADR-0042 D7")),
    (tag("impl", "REQ-DEMO-1"), ("malformed", "REQ-DEMO-1")),
])
def test_dangling_and_malformed_tags_are_gaps(tmp_path, bad_tag, gap):
    root = good_repo(tmp_path)
    (root / "mod/src/extra.c").write_text(bad_tag, encoding="utf-8")
    model = trace_report.build_model(root)
    assert gap in kinds(model)
    assert any(g.where == "mod/src/extra.c:1" for g in model.gaps)


def test_tags_without_an_id_are_ignored(tmp_path):
    root = good_repo(tmp_path)
    (root / "mod/src/extra.c").write_text(
        f"/* only on target ({AT}design-only). */\n/* see {AT}impl`/`{AT}test */\n",
        encoding="utf-8")
    assert trace_report.build_model(root).gaps == []


def test_bad_trace_to_missing_subdecision_is_a_gap(tmp_path):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D9", "`test_demo`", f"`{CAPTURE}`")]) + \
        body("REQ-DEMO-001")
    root = make_repo(tmp_path, md, GOOD_SOURCES)
    model = trace_report.build_model(root)
    assert ("bad-trace", "REQ-DEMO-001") in kinds(model)
    assert any("ADR-0042 D9" in g.detail for g in model.gaps)


def test_matrix_drift_when_named_test_lacks_the_tag(tmp_path):
    md = REQS_HEADER + matrix(
        [("REQ-DEMO-001", "D1", "`test_demo`, `test_other`, `test_ghost`", f"`{CAPTURE}`")]) + \
        body("REQ-DEMO-001")
    root = make_repo(tmp_path, md, GOOD_SOURCES,
                     extra={"mod/tests/test_other.c": "/* untagged */\n"})
    details = [g.detail for g in trace_report.build_model(root).gaps if g.kind == "matrix-drift"]
    assert len(details) == 2
    assert any("`test_other`" in d and "no test tag" in d for d in details)
    assert any("`test_ghost`" in d and "no such test file" in d for d in details)


def test_matrix_and_body_must_agree(tmp_path):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "`test_demo`", f"`{CAPTURE}`"),
                               ("REQ-DEMO-002", "D1", "", f"`{CAPTURE}`")]) + \
        body("REQ-DEMO-001", "REQ-DEMO-003")
    root = make_repo(tmp_path, md, GOOD_SOURCES)
    k = kinds(trace_report.build_model(root))
    assert ("matrix-only", "REQ-DEMO-002") in k
    assert ("body-only", "REQ-DEMO-003") in k


@pytest.mark.parametrize("cell, ok", [
    (f"`{CAPTURE}`", True),
    (f"`{CAPTURE}#stage-7`", True),
    ("n/a — host-side contract only", True),
    ("deferred — needs the M6 store", True),
    ("⏳", False),
    ("⏳ W43 Sat", False),
    ("", False),
    ("—", False),
    ("n/a", False),
    ("deferred", False),
    ("`docs/bench/2099-01-01/missing.txt`", False),
    ("passed on the bench", False),
])
def test_on_silicon_cell_rules(tmp_path, cell, ok):
    model = trace_report.build_model(good_repo(tmp_path, silicon=cell))
    assert (("no-silicon", "REQ-DEMO-001") not in kinds(model)) is ok


def test_deferrals_are_listed_in_the_report(tmp_path):
    model = trace_report.build_model(good_repo(tmp_path, silicon="deferred — needs M6"))
    report = trace_report.render(model)
    section = report.split("## Deferred on silicon", 1)[1]
    assert "**REQ-DEMO-001** — deferred — needs M6" in section


def test_missing_silicon_column_is_one_gap_per_file(tmp_path):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "`test_demo`", None)], silicon=False) + \
        body("REQ-DEMO-001")
    root = make_repo(tmp_path, md, GOOD_SOURCES)
    assert kinds(trace_report.build_model(root)) == [("no-silicon-col", "REQ-DEMO")]


def test_skipped_dirs_are_not_scanned(tmp_path):
    root = good_repo(tmp_path)
    for d in ("build", "vendor", "docs"):
        (root / d / "x.c").parent.mkdir(parents=True, exist_ok=True)
        (root / d / "x.c").write_text(tag("impl", "REQ-DEMO-999"), encoding="utf-8")
    assert trace_report.build_model(root).gaps == []


def test_python_sources_are_scanned(tmp_path):
    root = good_repo(tmp_path)
    (root / "tools" / "x.py").parent.mkdir()
    (root / "tools" / "x.py").write_text(f"# {AT}test REQ-DEMO-999 : dangling\n",
                                         encoding="utf-8")
    assert ("dangling", "REQ-DEMO-999") in kinds(trace_report.build_model(root))


def test_accepted_adr_without_tests_is_informational_only(tmp_path):
    root = good_repo(tmp_path)
    (root / "mod/tests/test_demo.c").write_text(tag("test", "REQ-DEMO-001"), encoding="utf-8")
    model = trace_report.build_model(root)
    assert model.gaps == []
    assert "| ADR-0042 | Demo decision | accepted | 1 | 0 | no test tag |" in \
        trace_report.render(model)


def test_main_writes_report_and_check_detects_staleness(tmp_path, capsys):
    root = good_repo(tmp_path)
    out = root / "docs" / "traceability.md"
    assert trace_report.main(["--root", str(root)]) == 0
    assert out.exists()
    assert trace_report.main(["--root", str(root), "--check"]) == 0

    (root / "mod/src/more.c").write_text(tag("impl", "REQ-DEMO-001"), encoding="utf-8")
    assert trace_report.main(["--root", str(root), "--check"]) == 1
    assert "STALE" in capsys.readouterr().out


def test_main_fails_on_gaps_and_gate_scopes_them(tmp_path, capsys):
    root = good_repo(tmp_path)
    (root / "mod/src/extra.c").write_text(tag("impl", "REQ-OTHER-001"), encoding="utf-8")
    assert trace_report.main(["--root", str(root)]) == 1
    assert "GAP  [dangling] REQ-OTHER-001" in capsys.readouterr().out
    assert trace_report.main(["--root", str(root), "--gate", "REQ-DEMO"]) == 0
    assert "info [dangling] REQ-OTHER-001" in capsys.readouterr().out


def test_report_is_deterministic(tmp_path):
    root = good_repo(tmp_path)
    assert trace_report.render(trace_report.build_model(root)) == \
        trace_report.render(trace_report.build_model(root))


# ---- "Observed by": how a requirement is seen on the bench (bench-testable rule) ----

EVENTS_CSV = """# fixture registry: id,name,class,arg0,arg1,description
0x0101,LOG_EVT_DEMO_OK,contract,can_id,-,demo verdict
0x0102,LOG_EVT_DEMO_SPARE,contract,-,-,a contract event no requirement names
0x1001,LOG_EVT_DEMO_DBG,diagnostic,value,tag,bring-up trace
"""


def observed_repo(tmp_path, observed, events=EVENTS_CSV):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "`test_demo`", f"`{CAPTURE}`", observed)]) +         body("REQ-DEMO-001")
    extra = {"shared/log/events.csv": events} if events is not None else {}
    return make_repo(tmp_path, md, GOOD_SOURCES, extra=extra)


@pytest.mark.parametrize("observed", [
    "`LOG_EVT_DEMO_OK`",
    "`LOG_EVT_DEMO_OK` (verdict per frame) + bus `0x120`",
    "bus `0x200` (can_sniff)",
    "structural — key bytes only in the CM0+ ELF",
    "debugger — `g_kat_result` == 15 in a CRYPTO_BRINGUP_KAT=1 build",
    "n/a — host-side contract",
])
def test_recognised_observations_are_not_gaps(tmp_path, observed):
    assert trace_report.build_model(observed_repo(tmp_path, observed)).gaps == []


@pytest.mark.parametrize("observed, detail", [
    ("", "no observation"),
    ("—", "no observation"),
    ("see the capture", "names no contract event"),
    ("structural", "without a reason"),
    ("n/a", "without a reason"),
    ("debugger", "without a reason"),
])
def test_missing_or_vague_observation_is_a_gap(tmp_path, observed, detail):
    model = trace_report.build_model(observed_repo(tmp_path, observed))
    gaps = [g for g in model.gaps if g.kind == "no-observed"]
    assert [g.id for g in gaps] == ["REQ-DEMO-001"]
    assert detail in gaps[0].detail


def test_an_unknown_event_is_dangling(tmp_path):
    model = trace_report.build_model(observed_repo(tmp_path, "`LOG_EVT_DEMO_TYPO`"))
    assert kinds(model) == [("dangling-event", "REQ-DEMO-001")]


def test_a_diagnostic_event_cannot_prove_a_requirement(tmp_path):
    """REQ-LOG-008: diagnostic IDs are never asserted, so they are no evidence."""
    model = trace_report.build_model(observed_repo(tmp_path, "`LOG_EVT_DEMO_DBG`"))
    assert kinds(model) == [("not-contract", "REQ-DEMO-001")]


def test_without_a_registry_every_event_is_dangling(tmp_path):
    model = trace_report.build_model(observed_repo(tmp_path, "`LOG_EVT_DEMO_OK`", events=None))
    assert kinds(model) == [("dangling-event", "REQ-DEMO-001")]


def test_a_silicon_column_without_an_observed_column_is_one_gap_per_file(tmp_path):
    md = REQS_HEADER + matrix([("REQ-DEMO-001", "D1", "`test_demo`", f"`{CAPTURE}`")],
                              observed=False) + body("REQ-DEMO-001")
    root = make_repo(tmp_path, md, GOOD_SOURCES)
    assert kinds(trace_report.build_model(root)) == [("no-observed-col", "REQ-DEMO")]


def test_report_shows_observed_and_lists_unclaimed_contract_events(tmp_path):
    """A contract event no requirement names is reported, not failed: it may be infrastructure."""
    model = trace_report.build_model(observed_repo(tmp_path, "`LOG_EVT_DEMO_OK`"))
    report = trace_report.render(model)
    assert "| Observed by |" in report and "`LOG_EVT_DEMO_OK`" in report
    section = report.split("## Contract events not named by a requirement", 1)[1]
    assert "`LOG_EVT_DEMO_SPARE`" in section and "LOG_EVT_DEMO_OK" not in section


# --- the real repo -------------------------------------------------------------------------

def test_no_tag_in_the_repo_names_an_undefined_requirement_or_decision():
    """Issue #37: a tag naming a REQ (or ADR, or ADR sub-decision) that does not exist
    is a broken trace in every area, tagged or not, so it fails here and not only in
    the report."""
    root = pathlib.Path(__file__).resolve().parents[2]
    dangling = [(g.id, g.where) for g in trace_report.build_model(root).gaps
                if g.kind == "dangling"]
    assert dangling == []
