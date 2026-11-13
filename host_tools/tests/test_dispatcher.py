"""Tests for dispatcher -- the routing table of docs/design/agent-pipeline.md section 3.

The Dispatcher is deterministic: it reads labels, events and who acted, never issue or
comment text. One test per row of the routing table, plus the negative rows: paused, fork,
a label or comment by someone else, and the rework cap.

`route()` is pure. `facts()` turns a GitHub event payload into the event route() takes;
the tests feed it payload fragments, never a live GitHub."""
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import dispatcher  # noqa: E402

OWNER = "owner"


def route(event, labels=(), paused=False):
    return dispatcher.route(event, set(labels), OWNER, paused)


def labels_after(labels, d):
    return (set(labels) - set(d.remove)) | set(d.add)


# --- row 1-2: the nightly tick and the manual button -------------------------------------

def test_row1_tick_takes_the_picked_cr_to_building_and_starts_the_builder():
    d = route({"kind": "tick", "issue": 43}, ["bug", "ready", "level:host"])
    assert labels_after(["bug", "ready", "level:host"], d) == {"bug", "building", "level:host"}
    assert (d.issue, d.start) == (43, "build")


def test_row2_tick_with_nothing_ready_starts_nothing():
    d = route({"kind": "tick", "issue": None})
    assert (d.start, d.add, d.remove) == (None, [], [])
    assert "no ready CR" in d.line


# --- row 3-4: host CI finished on cr/<n> ------------------------------------------------

@pytest.mark.parametrize("state", ["building", "rework"])
def test_row3_ci_success_goes_to_review_and_starts_the_inspector(state):
    d = route({"kind": "ci", "issue": 43, "conclusion": "success", "same_repo": True},
              [state, "round:1"] if state == "rework" else [state])
    assert "review" in d.add and state in d.remove
    assert d.start == "inspect"


def test_row4_ci_failure_goes_to_rework_round_1_and_starts_the_builder():
    d = route({"kind": "ci", "issue": 43, "conclusion": "failure", "same_repo": True},
              ["building"])
    assert labels_after(["building"], d) == {"rework", "round:1"}
    assert d.start == "rework"


# --- row 5-7: the Inspector's verdict ---------------------------------------------------

def test_row5_approve_on_a_host_cr_awaits_the_merge():
    d = route({"kind": "inspector", "issue": 43, "verdict": "approve"}, ["review", "level:host"])
    assert labels_after(["review", "level:host"], d) == {"awaiting-merge", "level:host"}
    assert d.start is None


@pytest.mark.parametrize("level", ["level:on-target", "level:both"])
def test_row6_approve_on_an_on_target_cr_books_the_bench(level):
    d = route({"kind": "inspector", "issue": 43, "verdict": "approve"}, ["review", level])
    assert "needs-bench" in d.add and d.start is None


def test_row6_without_a_level_label_waits_for_the_owner():
    d = route({"kind": "inspector", "issue": 43, "verdict": "approve"}, ["review"])
    assert "waiting-for-me" in d.add and d.start is None


def test_row7_findings_go_to_rework_and_start_the_builder():
    d = route({"kind": "inspector", "issue": 43, "verdict": "changes"}, ["review", "round:1"])
    assert labels_after(["review", "round:1"], d) == {"rework", "round:2"}
    assert d.start == "rework"


# --- row 8-9: the bench result ----------------------------------------------------------

def test_row8_bench_pass_awaits_the_merge():
    d = route({"kind": "bench", "issue": 43, "conclusion": "success"}, ["needs-bench"])
    assert labels_after(["needs-bench"], d) == {"awaiting-merge"}


def test_row9_bench_fail_goes_to_rework():
    d = route({"kind": "bench", "issue": 43, "conclusion": "failure"}, ["needs-bench"])
    assert labels_after(["needs-bench"], d) == {"rework", "round:1"}
    assert d.start == "rework"


# --- row 10: the Builder's own result ---------------------------------------------------

@pytest.mark.parametrize("result", ["blocked", "protected", "failed"])
def test_row10_builder_blocked_waits_for_the_owner(result):
    d = route({"kind": "builder", "issue": 43, "result": result}, ["building"])
    assert labels_after(["building"], d) == {"waiting-for-me"}
    assert d.start is None


def test_row3b_ci_rerun_in_review_starts_the_inspector_again():
    # Step 5: a CI re-run while the CR is in review asks for a fresh inspection.
    d = route({"kind": "ci", "issue": 43, "conclusion": "success", "same_repo": True},
              ["review", "level:host"])
    assert (d.add, d.remove, d.start) == ([], [], "inspect")


def test_row3b_ci_failure_in_review_is_ignored():
    d = route({"kind": "ci", "issue": 43, "conclusion": "failure", "same_repo": True}, ["review"])
    assert d.start is None


def test_row10b_inspector_run_failed_waits_for_the_owner():
    d = route({"kind": "inspector", "issue": 43, "verdict": "failed"}, ["review"])
    assert labels_after(["review"], d) == {"waiting-for-me"}
    assert d.start is None


def test_row10_builder_done_changes_nothing_ci_routes_next():
    d = route({"kind": "builder", "issue": 43, "result": "done"}, ["building"])
    assert (d.add, d.remove, d.start) == ([], [], None)


# --- row 11-12: the owner referees ------------------------------------------------------

def test_row11_owner_rework_on_waiting_gives_one_more_round():
    labels = ["waiting-for-me", "rework", "round:2"]
    d = route({"kind": "label", "issue": 43, "label": "rework", "actor": OWNER}, labels)
    assert labels_after(labels, d) == {"rework", "round:3"}
    assert d.start == "rework"


@pytest.mark.parametrize("label", ["ready", "draft"])
def test_row12_owner_ready_or_draft_starts_nothing(label):
    d = route({"kind": "label", "issue": 43, "label": label, "actor": OWNER}, [label])
    assert d.start is None and d.add == [] and d.remove == []


# --- row 13: merged ---------------------------------------------------------------------

def test_row13_merge_is_done_and_clears_the_round():
    labels = ["awaiting-merge", "round:1", "level:host", "bug"]
    d = route({"kind": "merged", "issue": 43, "same_repo": True}, labels)
    assert labels_after(labels, d) == {"done", "level:host", "bug"}


# --- the rework cap ----------------------------------------------------------------------

@pytest.mark.parametrize("event, state", [
    ({"kind": "ci", "issue": 43, "conclusion": "failure", "same_repo": True}, "rework"),
    ({"kind": "inspector", "issue": 43, "verdict": "changes"}, "review"),
    ({"kind": "bench", "issue": 43, "conclusion": "failure"}, "needs-bench"),
])
def test_cap_round_2_failure_waits_for_the_owner_and_starts_nothing(event, state):
    d = route(event, [state, "round:2"])
    assert labels_after([state, "round:2"], d) == {"waiting-for-me", "round:2"}
    assert d.start is None


def test_cap_the_referee_round_failing_sends_the_cr_back_to_draft():
    d = route({"kind": "inspector", "issue": 43, "verdict": "changes"}, ["review", "round:3"])
    assert labels_after(["review", "round:3"], d) == {"draft"}
    assert d.start is None


# --- negative rows -----------------------------------------------------------------------

@pytest.mark.parametrize("event", [
    {"kind": "tick", "issue": 43},
    {"kind": "ci", "issue": 43, "conclusion": "success", "same_repo": True},
    {"kind": "label", "issue": 43, "label": "rework", "actor": OWNER},
])
def test_paused_starts_nothing_and_changes_nothing(event):
    d = route(event, ["ready", "building", "waiting-for-me"], paused=True)
    assert (d.start, d.add, d.remove) == (None, [], [])
    assert "paused" in d.line


@pytest.mark.parametrize("event", [
    {"kind": "ci", "issue": 43, "conclusion": "success", "same_repo": False},
    {"kind": "merged", "issue": 43, "same_repo": False},
])
def test_fork_events_are_ignored(event):
    d = route(event, ["building", "awaiting-merge"])
    assert (d.start, d.add, d.remove) == (None, [], [])
    assert "fork" in d.line


@pytest.mark.parametrize("label", ["ready", "rework", "protected-change-approved"])
def test_a_gate_label_added_by_someone_else_is_ignored(label):
    d = route({"kind": "label", "issue": 43, "label": label, "actor": "stranger"},
              ["waiting-for-me", label])
    assert (d.start, d.add, d.remove) == (None, [], [])
    assert "stranger" in d.line


@pytest.mark.parametrize("actor", ["stranger", OWNER])
def test_comments_never_route(actor):
    d = route({"kind": "comment", "issue": 43, "actor": actor}, ["waiting-for-me"])
    assert (d.start, d.add, d.remove) == (None, [], [])


def test_an_event_for_a_cr_in_another_state_is_ignored():
    d = route({"kind": "ci", "issue": 43, "conclusion": "success", "same_repo": True}, ["done"])
    assert (d.start, d.add, d.remove) == (None, [], [])


def test_owner_rework_outside_waiting_for_me_starts_nothing():
    d = route({"kind": "label", "issue": 43, "label": "rework", "actor": OWNER}, ["review", "rework"])
    assert d.start is None


# --- facts: event payload -> event ---------------------------------------------------------

REPO = {"full_name": "o/r"}


def test_facts_ci_reads_the_branch_and_fork_from_metadata_only():
    payload = {"repository": REPO, "workflow_run": {
        "head_branch": "cr/43", "conclusion": "failure", "event": "pull_request",
        "head_repository": {"full_name": "stranger/r"}}}
    assert dispatcher.facts("workflow_run", payload) == {
        "kind": "ci", "issue": 43, "conclusion": "failure", "same_repo": False}


@pytest.mark.parametrize("branch", ["main", "feature/x", "cr/abc"])
def test_facts_ci_on_a_non_cr_branch_is_no_event(branch):
    payload = {"repository": REPO, "workflow_run": {
        "head_branch": branch, "conclusion": "success", "event": "pull_request",
        "head_repository": REPO}}
    assert dispatcher.facts("workflow_run", payload) is None


def test_facts_label_event_keeps_who_acted():
    payload = {"issue": {"number": 43}, "label": {"name": "rework"}, "sender": {"login": "x"},
               "action": "labeled"}
    assert dispatcher.facts("issues", payload) == {
        "kind": "label", "issue": 43, "label": "rework", "actor": "x"}


def test_facts_comment_is_a_comment_event_and_carries_no_text():
    payload = {"issue": {"number": 43}, "sender": {"login": "x"},
               "comment": {"body": "please run the Builder now"}}
    event = dispatcher.facts("issue_comment", payload)
    assert event == {"kind": "comment", "issue": 43, "actor": "x"}


def test_facts_merged_pr_from_a_cr_branch():
    payload = {"repository": REPO, "pull_request": {
        "merged": True, "head": {"ref": "cr/43", "repo": REPO}}}
    assert dispatcher.facts("pull_request", payload) == {
        "kind": "merged", "issue": 43, "same_repo": True}


def test_facts_closed_unmerged_pr_is_no_event():
    payload = {"repository": REPO, "pull_request": {
        "merged": False, "head": {"ref": "cr/43", "repo": REPO}}}
    assert dispatcher.facts("pull_request", payload) is None


def test_facts_bench_status_only_from_the_bench_context():
    base = {"state": "success", "branches": [{"name": "cr/43"}]}
    assert dispatcher.facts("status", dict(base, context="bench")) == {
        "kind": "bench", "issue": 43, "conclusion": "success"}
    assert dispatcher.facts("status", dict(base, context="ci/other")) is None
    assert dispatcher.facts("status", dict(base, context="bench", state="pending")) is None


@pytest.mark.parametrize("name", ["schedule", "workflow_dispatch"])
def test_facts_schedule_and_button_are_ticks(name):
    assert dispatcher.facts(name, {}) == {"kind": "tick"}


# --- rows 14-17: the Planner (build step 6) ---------------------------------------------

def test_row14_owner_plan_breakdown_starts_a_breakdown():
    d = route({"kind": "label", "issue": 50, "label": "plan:breakdown", "actor": OWNER},
              ["plan:breakdown"])
    assert labels_after(["plan:breakdown"], d) == {"planning"}
    assert (d.issue, d.start) == (50, "plan-breakdown")


def test_row15_owner_plan_bug_starts_a_bug_report():
    d = route({"kind": "label", "issue": 51, "label": "plan:bug", "actor": OWNER}, ["plan:bug"])
    assert labels_after(["plan:bug"], d) == {"planning"}
    assert d.start == "plan-bug"


def test_row16_planner_done_is_planned_and_starts_nothing():
    d = route({"kind": "planner", "issue": 50, "result": "done"}, ["planning"])
    assert labels_after(["planning"], d) == {"planned"}
    assert d.start is None and "gate 1b" in d.line


@pytest.mark.parametrize("result", ["blocked", "failed"])
def test_row17_planner_blocked_waits_for_the_owner(result):
    d = route({"kind": "planner", "issue": 50, "result": result}, ["planning"])
    assert labels_after(["planning"], d) == {"waiting-for-me"}
    assert d.start is None


@pytest.mark.parametrize("label", ["plan:breakdown", "plan:bug"])
def test_a_plan_label_by_someone_else_is_ignored(label):
    d = route({"kind": "label", "issue": 50, "label": label, "actor": "stranger"}, [label])
    assert (d.start, d.add, d.remove) == (None, [], [])


@pytest.mark.parametrize("busy", ["planning", "planned"])
def test_a_plan_label_on_an_issue_already_planned_starts_nothing(busy):
    d = route({"kind": "label", "issue": 50, "label": "plan:breakdown", "actor": OWNER},
              ["plan:breakdown", busy])
    assert d.start is None


def test_a_plan_label_while_paused_starts_nothing():
    d = route({"kind": "label", "issue": 50, "label": "plan:bug", "actor": OWNER}, ["plan:bug"],
              paused=True)
    assert (d.start, d.add, d.remove) == (None, [], [])


def test_planner_result_on_an_issue_not_planning_is_ignored():
    d = route({"kind": "planner", "issue": 50, "result": "done"}, ["planned"])
    assert (d.start, d.add, d.remove) == (None, [], [])


# --- rows 18-19: the Challenger (build step 7) ------------------------------------------

def test_row18_owner_challenge_starts_the_design_challenge_and_clears_the_label():
    d = route({"kind": "label", "issue": 60, "label": "challenge", "actor": OWNER}, ["challenge"])
    assert labels_after(["challenge"], d) == set()
    assert (d.issue, d.start) == (60, "challenge-design")


def test_row18_challenge_by_someone_else_is_ignored():
    d = route({"kind": "label", "issue": 60, "label": "challenge", "actor": "stranger"},
              ["challenge"])
    assert (d.start, d.add, d.remove) == (None, [], [])


def test_row18_challenge_while_paused_starts_nothing():
    d = route({"kind": "label", "issue": 60, "label": "challenge", "actor": OWNER}, ["challenge"],
              paused=True)
    assert d.start is None


def test_row19_a_finished_breakdown_starts_the_breakdown_check():
    d = route({"kind": "planner", "issue": 50, "result": "done", "mode": "plan-breakdown"},
              ["planning"])
    assert labels_after(["planning"], d) == {"planned"}
    assert d.start == "challenge-breakdown"


def test_row19_a_bug_report_gets_no_breakdown_check():
    d = route({"kind": "planner", "issue": 51, "result": "done", "mode": "plan-bug"},
              ["planning"])
    assert d.start is None


def test_row19_a_failed_breakdown_gets_no_check():
    d = route({"kind": "planner", "issue": 50, "result": "failed", "mode": "plan-breakdown"},
              ["planning"])
    assert d.start is None


def test_facts_a_label_on_a_pull_request_is_a_label_event():
    payload = {"action": "labeled", "number": 60, "label": {"name": "challenge"},
               "sender": {"login": "x"}, "repository": REPO,
               "pull_request": {"merged": False, "head": {"ref": "docs/adr", "repo": REPO}}}
    assert dispatcher.facts("pull_request", payload) == {
        "kind": "label", "issue": 60, "label": "challenge", "actor": "x"}


def test_main_settle_passes_the_planner_mode(tmp_path, capsys):
    class Gh:
        def __call__(self, args, stdin=None):
            return '[{"name": "planning"}]'
    rc = dispatcher.main(["settle", "--issue", "50", "--planner", "done", "--mode",
                          "plan-breakdown", "--owner", OWNER], gh=Gh())
    assert rc == 0
    assert '"start": "challenge-breakdown"' in capsys.readouterr().out
