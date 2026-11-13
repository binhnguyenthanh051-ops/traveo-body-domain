"""Tests for cr_queue -- which CR the Builder takes, and whether the owner set a label.

Design note docs/design/agent-pipeline.md: section 3 (the Builder takes the oldest `ready`
CR, one per run), section 4 ("agent-ready" = every field filled and the owner set `ready`),
section 7 (agents take instructions only from the owner) and ADR-0026 (an app token can
add labels, so a label counts only when the owner added it).

No test talks to GitHub: the tool takes its `gh` runner as an argument."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import cr_queue  # noqa: E402

OWNER = "owner"
BOT = "traveo-builder[bot]"

CR_BODY = """### Type

change

### What and why

Make the thing do the other thing.

### ADR link

docs/architecture/decisions/ADR-0023-target-logging.md, D13

### Requirement IDs

REQ-LOG-015

### Acceptance criteria

- [ ] it does the other thing

### Test level

host

### Milestone

M6
"""

BUG_BODY = """### Observed

it crashed

### Expected

it should not crash

### How to reproduce

make test

### Affected requirement

REQ-LOG-001

### Test level

host

### Milestone

M6

---

**Planner note:** anything after the fields is free text.
"""


def issue(number, body=CR_BODY, labels=("cr", "ready"), author=OWNER, pr=False):
    item = {"number": number, "title": f"CR {number}", "body": body,
            "user": {"login": author}, "created_at": f"2026-11-{number:02d}T00:00:00Z",
            "labels": [{"name": n} for n in labels]}
    if pr:
        item["pull_request"] = {"url": "x"}
    return item


def labeled(label, actor, event="labeled"):
    return {"event": event, "label": {"name": label}, "actor": {"login": actor}}


class FakeGh:
    """Answers the issue list and each issue's timeline."""

    def __init__(self, issues, timelines=None):
        self.issues = issues
        self.timelines = timelines or {}
        self.calls = []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        path = next(a for a in args if a.startswith("repos/"))
        if "/timeline" in path:
            number = int(path.split("/issues/")[1].split("/")[0])
            return json.dumps(self.timelines.get(number, [labeled("ready", OWNER)]))
        return json.dumps(self.issues)

    def writes(self):
        return [c for c in self.calls if "--method" in c]


# --- the fields -----------------------------------------------------------------------

def test_a_complete_cr_has_no_missing_field():
    assert cr_queue.missing_fields(CR_BODY, "cr") == []


def test_a_complete_bug_report_has_no_missing_field_and_free_text_after_is_fine():
    assert cr_queue.missing_fields(BUG_BODY, "bug") == []


@pytest.mark.parametrize("empty", ["", "_No response_", "   "])
def test_an_empty_field_is_missing(empty):
    body = CR_BODY.replace("- [ ] it does the other thing", empty)
    assert cr_queue.missing_fields(body, "cr") == ["Acceptance criteria"]


def test_a_field_that_is_not_there_at_all_is_missing():
    body = CR_BODY.replace("### Requirement IDs\n\nREQ-LOG-015\n\n", "")
    assert cr_queue.missing_fields(body, "cr") == ["Requirement IDs"]


def test_crlf_bodies_parse_the_same():
    assert cr_queue.missing_fields(CR_BODY.replace("\n", "\r\n"), "cr") == []


# --- who set the label ----------------------------------------------------------------

def test_label_added_by_the_owner_counts():
    assert cr_queue.last_labeler([labeled("ready", OWNER)], "ready") == OWNER


def test_the_last_add_wins():
    events = [labeled("ready", OWNER), labeled("ready", OWNER, "unlabeled"),
              labeled("ready", BOT)]
    assert cr_queue.last_labeler(events, "ready") == BOT


def test_a_removed_label_has_no_labeler():
    events = [labeled("ready", OWNER), labeled("ready", OWNER, "unlabeled")]
    assert cr_queue.last_labeler(events, "ready") is None


def test_other_labels_and_events_are_ignored():
    events = [labeled("ready", OWNER), labeled("bug", BOT), {"event": "commented"}]
    assert cr_queue.last_labeler(events, "ready") == OWNER


# --- pick: the oldest agent-ready CR ----------------------------------------------------

def pick(issues, timelines=None):
    return cr_queue.pick(FakeGh(issues, timelines), OWNER)


def test_the_oldest_ready_cr_is_picked():
    chosen, skipped = pick([issue(9), issue(3), issue(5)])
    assert chosen == 3 and skipped == []


def test_a_bug_report_is_picked_with_its_own_fields():
    chosen, _ = pick([issue(4, body=BUG_BODY, labels=("bug", "ready"))])
    assert chosen == 4


def test_nothing_ready_means_no_pick():
    assert pick([]) == (None, [])


def test_pull_requests_in_the_list_are_ignored():
    chosen, skipped = pick([issue(2, pr=True), issue(6)])
    assert chosen == 6 and skipped == []


@pytest.mark.parametrize("bad, reason", [
    (issue(1, author="stranger"), "not written by the owner"),
    (issue(1, labels=("ready",)), "neither cr nor bug"),
    (issue(1, body=CR_BODY.replace("host", "")), "empty field: Test level"),
])
def test_a_cr_that_is_not_agent_ready_is_skipped_with_a_reason(bad, reason):
    chosen, skipped = pick([bad, issue(8)])
    assert chosen == 8
    assert skipped == [(1, reason)]


def test_ready_set_by_an_app_is_skipped():
    chosen, skipped = pick([issue(1), issue(8)], {1: [labeled("ready", BOT)]})
    assert chosen == 8
    assert skipped == [(1, f"ready was set by {BOT}, not the owner")]


def test_pick_never_writes_to_github():
    gh = FakeGh([issue(1)])
    cr_queue.pick(gh, OWNER)
    assert gh.writes() == []


# --- main -------------------------------------------------------------------------------

def test_main_pick_prints_json_for_the_workflow(capsys):
    gh = FakeGh([issue(1, author="stranger"), issue(8)])
    assert cr_queue.main(["pick", "--owner", OWNER], gh=gh) == 0
    out = json.loads(capsys.readouterr().out)
    assert out == {"issue": 8, "skipped": [[1, "not written by the owner"]]}


def test_main_pick_with_nothing_ready_prints_null(capsys):
    assert cr_queue.main(["pick", "--owner", OWNER], gh=FakeGh([])) == 0
    assert json.loads(capsys.readouterr().out) == {"issue": None, "skipped": []}


@pytest.mark.parametrize("timeline, expected", [
    ([labeled("protected-change-approved", OWNER)], 0),
    ([labeled("protected-change-approved", BOT)], 1),
    ([], 1),
])
def test_main_label_check_passes_only_when_the_owner_added_the_label(timeline, expected, capsys):
    gh = FakeGh([], {38: timeline})
    rc = cr_queue.main(["label-by-owner", "--owner", OWNER, "--issue", "38",
                        "--label", "protected-change-approved"], gh=gh)
    assert rc == expected
