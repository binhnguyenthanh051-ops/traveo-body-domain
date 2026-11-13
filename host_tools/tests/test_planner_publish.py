"""Tests for planner_publish -- turns the Planner's plan.json into draft issues (build step 6).

The Planner is an agent; the publisher is a script. It checks the plan against the issue-form
contract (every field of the form, filled), refuses anything else, and only then creates the
issues as `draft`, plus one summary comment on the source issue: the list the owner approves
at gate 1b (docs/design/agent-pipeline.md sections 4-5)."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import planner_publish  # noqa: E402

CR_FIELDS = {"Type": "new", "What and why": "w", "ADR link": "ADR-0023 D13",
             "Requirement IDs": "REQ-LOG-015", "Acceptance criteria": "- [ ] a\n- [ ] b",
             "Test level": "host", "Milestone": "M6"}
BUG_FIELDS = {"Observed": "o", "Expected": "e", "How to reproduce": "r",
              "Affected requirement": "none: hygiene", "Test level": "host", "Milestone": "M6"}


def plan(*issues, status="done", summary="two CRs from ADR-0027"):
    return {"status": status, "summary": summary, "issues": list(issues)}


def cr(title="CR: one", **fields):
    return {"kind": "cr", "title": title, "fields": dict(CR_FIELDS, **fields)}


def bug(title="Bug: one", **fields):
    return {"kind": "bug", "title": title, "fields": dict(BUG_FIELDS, **fields)}


# --- validate: the form contract --------------------------------------------------------

def test_a_complete_plan_is_valid():
    assert planner_publish.validate(plan(cr(), bug())) == []


@pytest.mark.parametrize("bad, problem", [
    (cr(**{"Acceptance criteria": ""}), "empty field 'Acceptance criteria'"),
    (cr(**{"Test level": "maybe"}), "Test level must be host, on-target or both"),
    ({"kind": "epic", "title": "x", "fields": {}}, "kind must be cr or bug"),
    (dict(cr(), title=""), "no title"),
])
def test_an_issue_that_breaks_the_form_is_refused(bad, problem):
    assert any(problem in p for p in planner_publish.validate(plan(bad)))


def test_a_missing_field_is_refused():
    item = cr()
    del item["fields"]["ADR link"]
    assert any("missing field 'ADR link'" in p for p in planner_publish.validate(plan(item)))


def test_an_unknown_field_is_refused():
    assert any("unknown field" in p
               for p in planner_publish.validate(plan(cr(**{"Priority": "high"}))))


def test_too_many_issues_is_refused():
    assert any("at most" in p for p in planner_publish.validate(plan(*[cr()] * 16)))


def test_a_done_plan_with_no_issues_is_refused():
    assert planner_publish.validate(plan()) != []


# --- body: the form's headings, in the form's order --------------------------------------

def test_the_body_uses_the_form_headings_in_order():
    body = planner_publish.body(cr())
    headings = [l[4:] for l in body.splitlines() if l.startswith("### ")]
    assert headings == list(CR_FIELDS)
    assert "### Acceptance criteria\n\n- [ ] a\n- [ ] b\n" in body


# --- main: creates drafts and one summary comment ------------------------------------------

class FakeGh:
    def __init__(self):
        self.calls, self.stdin = [], []
        self.next = 60

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        self.stdin.append(stdin)
        if args[:2] == ["issue", "create"]:
            self.next += 1
            return f"https://github.com/o/r/issues/{self.next}\n"
        return ""


def write(tmp_path, data):
    p = tmp_path / "plan.json"
    p.write_text(json.dumps(data), encoding="utf-8")
    return str(p)


def test_main_creates_draft_issues_and_one_summary_comment(tmp_path, capsys):
    gh = FakeGh()
    rc = planner_publish.main([write(tmp_path, plan(cr("CR: a"), bug("Bug: b"))),
                               "--source", "50"], gh=gh)
    assert rc == 0
    creates = [c for c in gh.calls if c[:2] == ["issue", "create"]]
    assert len(creates) == 2
    assert creates[0][creates[0].index("--label") + 1] == "draft"
    assert "cr" in creates[0] and "bug" in creates[1]
    assert creates[0][creates[0].index("--milestone") + 1] == "M6"
    (comment,) = [c for c in gh.calls if c[:2] == ["issue", "comment"]]
    assert comment[2] == "50"
    text = gh.stdin[gh.calls.index(comment)]
    assert "#61" in text and "#62" in text and "- [ ] a" in text
    assert capsys.readouterr().out.strip() == "result=done"


def test_main_refuses_an_invalid_plan_and_creates_nothing(tmp_path, capsys):
    gh = FakeGh()
    rc = planner_publish.main([write(tmp_path, plan(cr(**{"Milestone": ""}))), "--source", "50"],
                              gh=gh)
    assert rc == 0
    assert [c for c in gh.calls if c[:2] == ["issue", "create"]] == []
    (comment,) = [c for c in gh.calls if c[:2] == ["issue", "comment"]]
    assert "empty field 'Milestone'" in gh.stdin[gh.calls.index(comment)]
    assert capsys.readouterr().out.strip() == "result=blocked"


def test_main_passes_on_a_blocked_plan(tmp_path, capsys):
    gh = FakeGh()
    planner_publish.main([write(tmp_path, plan(status="blocked", summary="which ADR?")),
                          "--source", "50"], gh=gh)
    assert [c for c in gh.calls if c[:2] == ["issue", "create"]] == []
    assert capsys.readouterr().out.strip() == "result=blocked"


def test_main_with_no_plan_file_is_failed(tmp_path, capsys):
    gh = FakeGh()
    planner_publish.main([str(tmp_path / "missing.json"), "--source", "50"], gh=gh)
    assert capsys.readouterr().out.strip() == "result=failed"
