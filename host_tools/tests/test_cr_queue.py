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


READY_AT = "2026-11-13T10:00:00Z"


def labeled(label, actor, event="labeled", at=READY_AT):
    return {"event": event, "label": {"name": label}, "actor": {"login": actor}, "created_at": at}


def renamed(actor, at):
    return {"event": "renamed", "actor": {"login": actor}, "created_at": at}


def edit(editor, at):
    return {"editedAt": at, "editor": {"login": editor} if editor else None}


class FakeGh:
    """Answers the issue list and each issue's timeline."""

    def __init__(self, issues, timelines=None, edits=None):
        self.issues = issues
        self.timelines = timelines or {}
        self.edits = edits or {}
        self.calls = []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        if args[:2] == ["api", "graphql"]:
            number = int(next(a for a in args if a.startswith("number=")).split("=")[1])
            return json.dumps({"data": {"repository": {"issue": {"userContentEdits": {
                "nodes": self.edits.get(number, [])}}}}})
        path = next(a for a in args if a.startswith("repos/"))
        if "/timeline" in path:
            number = int(path.split("/issues/")[1].split("/")[0])
            return json.dumps(self.timelines.get(number, [labeled("ready", OWNER)]))
        return json.dumps(self.issues)

    def writes(self):
        return [c for c in self.calls if "--method" in c]


# --- no text is read ---------------------------------------------------------------

def test_pick_reads_no_issue_text():
    # The Dispatcher reads labels, events and who acted, never text (design note section 3).
    # An empty form field is the Builder's to report, as `blocked`.
    chosen, skipped = pick([issue(1, body="")])
    assert chosen == 1 and skipped == []


# --- who set the label ----------------------------------------------------------------

# @test ADR-0026 D5 : the owner's label counts, anyone else's does not
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
    (issue(1, author="stranger"), "not written by the owner or the Planner"),
    (issue(1, labels=("ready",)), "neither cr nor bug"),
])
def test_a_cr_that_is_not_agent_ready_is_skipped_with_a_reason(bad, reason):
    chosen, skipped = pick([bad, issue(8)])
    assert chosen == 8
    assert skipped == [(1, reason)]


PLANNER = "reviewer-github-app[bot]"


def test_a_cr_written_by_the_planner_is_picked_when_the_owner_set_ready():
    chosen, skipped = cr_queue.pick(FakeGh([issue(1, author=PLANNER)]), OWNER, planner=PLANNER)
    assert chosen == 1 and skipped == []


def test_a_cr_written_by_the_planner_with_ready_by_the_planner_is_skipped():
    gh = FakeGh([issue(1, author=PLANNER)], {1: [labeled("ready", PLANNER)]})
    chosen, skipped = cr_queue.pick(gh, OWNER, planner=PLANNER)
    assert chosen is None
    assert skipped == [(1, f"ready was set by {PLANNER}, not the owner")]


def test_without_a_planner_login_only_the_owner_counts_as_author():
    chosen, skipped = cr_queue.pick(FakeGh([issue(1, author=PLANNER)]), OWNER)
    assert chosen is None and skipped == [(1, "not written by the owner or the Planner")]


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
    assert out == {"issue": 8, "skipped": [[1, "not written by the owner or the Planner"]]}


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


# --- the issue text is pinned at `ready` (Challenger finding on #52) -------------------------
# An app can edit an issue after the owner approved it. The check reads who edited and when,
# never the text itself.

LATER, EARLIER = "2026-11-13T11:00:00Z", "2026-11-13T09:00:00Z"


@pytest.mark.parametrize("events, edits, expected", [
    ([labeled("ready", OWNER)], [], []),
    ([labeled("ready", OWNER)], [edit(BOT, LATER)], [(BOT, "text")]),
    ([labeled("ready", OWNER)], [edit(BOT, EARLIER)], []),
    ([labeled("ready", OWNER)], [edit(OWNER, LATER)], []),
    ([labeled("ready", OWNER)], [edit(None, LATER)], [("(deleted user)", "text")]),
    ([labeled("ready", OWNER), renamed(BOT, LATER)], [], [(BOT, "title")]),
    ([renamed(BOT, EARLIER), labeled("ready", OWNER)], [], []),
    # Re-approving after the edit pins the new text: only edits after the last `ready` count.
    ([labeled("ready", OWNER, at=EARLIER), labeled("ready", OWNER, "unlabeled", at=LATER),
      labeled("ready", OWNER, at="2026-11-13T12:00:00Z")], [edit(BOT, LATER)], []),
])
def test_edits_after_ready(events, edits, expected):
    assert cr_queue.edited_after_ready(events, edits, OWNER) == expected


def test_pick_skips_a_cr_whose_text_an_app_edited_after_ready():
    gh = FakeGh([issue(1), issue(8)], edits={1: [edit(BOT, LATER)]})
    chosen, skipped = cr_queue.pick(gh, OWNER)
    assert chosen == 8
    assert skipped == [(1, f"text edited by {BOT} after ready")]


@pytest.mark.parametrize("edits, expected", [([], 0), ([edit(BOT, LATER)], 1)])
def test_main_text_unchanged_check(edits, expected, capsys):
    gh = FakeGh([], {43: [labeled("ready", OWNER)]}, edits={43: edits})
    assert cr_queue.main(["text-unchanged", "--owner", OWNER, "--issue", "43"], gh=gh) == expected
