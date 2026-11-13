"""Tests for release_check -- the release gate against the Definition of Done (build step 9).

DoD rule 11 in docs/project/plan.md is the source of truth. The section 11 items not yet in it
(every CR done, the on-target regression on the release commit, release notes naming every CR)
are checked but report-only until the owner adds them to the DoD; then one line of config makes
them blocking. Manual items (the post reviewed, no architecture-doc drift) need the owner's
tick in the release form."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import release_check  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"

REVIEW = """# Review record — M6

## 1. Requirements: complete, testable, unambiguous

**Reviewed:** 2026-12-01. **Verdict:** **pass.**

## 2. Design: each ADR decision covers its REQs

**Verdict:** **pass with findings.**

## 3. Tests: each @test checks what its REQ says

**Verdict:** **pass.**
"""


class Gh:
    def __init__(self, open_issues=(), done=(61, 62), statuses=None, releases=()):
        self.open, self.done = list(open_issues), list(done)
        self.statuses = [{"context": "bench-release", "state": "success"}] if statuses is None else statuses
        self.releases = list(releases)

    def __call__(self, args, stdin=None):
        path = next(a for a in args if a.startswith("repos/"))
        if "/milestones" in path:
            return json.dumps([{"number": 7, "title": "M6"}])
        if "/issues" in path and "state=open" in path:
            return json.dumps([{"number": n} for n in self.open])
        if "/issues" in path:
            return json.dumps([{"number": n, "labels": [{"name": "done"}, {"name": "cr"}]}
                               for n in self.done])
        if "/statuses" in path:
            return json.dumps(self.statuses)
        if "/releases" in path:
            return json.dumps([{"tag_name": t} for t in self.releases])
        raise AssertionError(path)


@pytest.fixture
def repo(tmp_path):
    (tmp_path / "docs" / "releases").mkdir(parents=True)
    (tmp_path / "docs" / "review").mkdir(parents=True)
    (tmp_path / "docs" / "releases" / "m6-diag.md").write_text(
        "# M6\n\n- #61 first CR\n- #62 second CR\n", encoding="utf-8")
    (tmp_path / "docs" / "review" / "M6-review.md").write_text(REVIEW, encoding="utf-8")
    return tmp_path


def check(repo, gh=None, manual=True, trace_rc=0, report_only=("milestone_done",
          "bench_release", "release_notes_crs"), known=("m5b-secoc",)):
    return release_check.run_checks(
        tag="m6-diag", milestone="M6", sha=SHA, root=repo, gh=gh or Gh(),
        trace=lambda: trace_rc, manual_confirmed=manual,
        report_only=set(report_only), known_releases=set(known))


def by_name(results):
    return {r.name: r for r in results}


def test_a_ready_release_passes(repo):
    results = check(repo)
    assert release_check.blocking_failures(results) == []
    assert by_name(results)["security"].state == "not yet"


@pytest.mark.parametrize("name, kwargs", [
    ("traceability", {"trace_rc": 1}),
    ("manual", {"manual": False}),
    ("foreign_release", {"gh": Gh(releases=["m5b-secoc", "m6-sneaky"])}),
])
def test_a_blocking_check_failing_blocks_the_release(repo, name, kwargs):
    results = check(repo, **kwargs)
    assert name in [r.name for r in release_check.blocking_failures(results)]


def test_missing_release_notes_block(repo):
    (repo / "docs" / "releases" / "m6-diag.md").unlink()
    assert "release_notes" in [r.name for r in release_check.blocking_failures(check(repo))]


@pytest.mark.parametrize("text, problem", [
    (REVIEW.replace("## 3. Tests", "## 3. Other"), "Tests"),
    (REVIEW.replace("**Verdict:** **pass with findings.**", "pending"), "Design"),
])
def test_an_incomplete_review_record_blocks(repo, text, problem):
    (repo / "docs" / "review" / "M6-review.md").write_text(text, encoding="utf-8")
    failed = {r.name: r for r in release_check.blocking_failures(check(repo))}
    assert "review_record" in failed and problem in failed["review_record"].detail


def test_the_review_record_of_a_letter_milestone_falls_back_to_its_number(repo):
    (repo / "docs" / "releases" / "m6b-diag.md").write_text("- #61\n- #62\n", encoding="utf-8")
    results = release_check.run_checks(
        tag="m6b-diag", milestone="M6", sha=SHA, root=repo, gh=Gh(), trace=lambda: 0,
        manual_confirmed=True, report_only=set(), known_releases={"m5b-secoc"})
    assert by_name(results)["review_record"].state == "pass"


@pytest.mark.parametrize("name, gh", [
    ("milestone_done", Gh(open_issues=[63])),
    ("bench_release", Gh(statuses=[])),
    ("bench_release", Gh(statuses=[{"context": "bench-release", "state": "failure"}])),
    ("release_notes_crs", Gh(done=[61, 62, 64])),
])
def test_section_11_items_fail_but_only_report_until_they_are_in_the_dod(repo, name, gh):
    results = check(repo, gh=gh)
    assert by_name(results)[name].state == "fail"
    assert name not in [r.name for r in release_check.blocking_failures(results)]
    # ...and block once the owner moves them into the DoD (empty report-only list):
    assert name in [r.name for r in release_check.blocking_failures(check(repo, gh=gh, report_only=()))]


def test_the_real_config_keeps_the_section_11_items_report_only():
    config = release_check.load_config(release_check.DEFAULT_CONFIG)
    assert set(config["report_only"]) == {"milestone_done", "bench_release", "release_notes_crs"}
    assert "m5b-secoc" in config["known_releases"]


def test_main_prints_a_table_and_exits_1_on_a_blocking_failure(repo, capsys, monkeypatch):
    monkeypatch.setattr(release_check, "run_trace", lambda root, area: 1)
    rc = release_check.main(["--tag", "m6-diag", "--milestone", "M6", "--sha", SHA,
                             "--area", "REQ-DIAG", "--root", str(repo)], gh=Gh())
    out = capsys.readouterr().out
    assert rc == 1
    assert "traceability" in out and "FAIL" in out
