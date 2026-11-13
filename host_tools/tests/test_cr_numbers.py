"""Tests for cr_numbers -- the three numbers per CR (docs/design/agent-pipeline.md section 8).

Rework rounds, time from the owner's first `ready` to the merge, and referee calls. All three
come from the label events in the issue timeline, never from issue or comment text, so the
Dispatcher can log them and the numbers in the blog can be recomputed at any time.

The fixtures are the real label timelines of CR #37 (walked through by hand) and CR #43 (the
first automatic CR), cut down to the events that count."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import cr_numbers  # noqa: E402

OWNER = "owner"
BOT = "github-actions[bot]"


def ev(when, event, actor="", label=None):
    e = {"event": event, "created_at": f"2026-11-13T{when}Z", "actor": {"login": actor}}
    if label:
        e["label"] = {"name": label}
    return e


# CR #43, 2026-11-13: three Builder blocks and two failed Inspector runs while the apps were
# being set up, then approve and merge. No rework round.
CR43 = [
    ev("11:07:47", "labeled", OWNER, "draft"),
    ev("11:08:20", "unlabeled", OWNER, "draft"),
    ev("11:08:20", "labeled", OWNER, "ready"),
    ev("11:08:53", "unlabeled", BOT, "ready"),
    ev("11:08:53", "labeled", BOT, "building"),
    ev("11:57:48", "labeled", OWNER, "ready"),
    ev("11:58:22", "labeled", BOT, "building"),
    ev("12:00:16", "labeled", BOT, "waiting-for-me"),
    ev("12:06:44", "labeled", OWNER, "ready"),
    ev("12:06:44", "unlabeled", OWNER, "waiting-for-me"),
    ev("12:08:58", "labeled", BOT, "waiting-for-me"),
    ev("12:10:04", "unlabeled", OWNER, "waiting-for-me"),
    ev("12:12:11", "labeled", BOT, "waiting-for-me"),
    ev("12:17:48", "unlabeled", OWNER, "waiting-for-me"),
    ev("12:17:48", "labeled", OWNER, "ready"),
    ev("12:21:49", "labeled", BOT, "review"),
    ev("12:47:01", "labeled", BOT, "waiting-for-me"),
    ev("12:50:04", "unlabeled", OWNER, "waiting-for-me"),
    ev("12:55:34", "labeled", BOT, "waiting-for-me"),
    ev("12:59:53", "unlabeled", OWNER, "waiting-for-me"),
    ev("13:02:59", "labeled", BOT, "awaiting-merge"),
    ev("13:08:38", "closed", OWNER),
    ev("13:08:52", "labeled", BOT, "done"),
]

# CR #37, 2026-11-13 (times after midnight UTC): every stage moved by hand, one rework, and
# no round:N label, because the Dispatcher did not exist yet.
CR37 = [
    ev("00:00:17", "labeled", OWNER, "ready"),
    ev("00:04:47", "labeled", OWNER, "building"),
    ev("00:07:30", "labeled", OWNER, "review"),
    ev("00:11:25", "labeled", OWNER, "rework"),
    ev("00:12:18", "labeled", OWNER, "building"),
    ev("00:13:51", "labeled", OWNER, "review"),
    ev("00:17:11", "labeled", OWNER, "awaiting-merge"),
    ev("00:19:00", "closed", OWNER),
    ev("00:19:15", "labeled", OWNER, "done"),
]


def test_cr43_measured():
    n = cr_numbers.numbers(CR43, OWNER)
    assert n == {"rework_rounds": 0, "ready_to_merge_min": 120, "referee_calls": 5}


def test_cr37_measured_a_manual_rework_counts_as_a_round():
    n = cr_numbers.numbers(CR37, OWNER)
    assert n == {"rework_rounds": 1, "ready_to_merge_min": 19, "referee_calls": 0}


def test_the_clock_starts_at_the_owners_first_ready_not_the_last():
    # #43: first ready 11:08:20, last 12:17:48; the referee waits are part of the cost.
    assert cr_numbers.numbers(CR43, OWNER)["ready_to_merge_min"] == 120


def test_ready_added_by_someone_else_does_not_start_the_clock():
    events = [ev("10:00:00", "labeled", "stranger", "ready"),
              ev("10:30:00", "labeled", OWNER, "ready"),
              ev("11:00:00", "closed", OWNER)]
    assert cr_numbers.numbers(events, OWNER)["ready_to_merge_min"] == 30


def test_rework_rounds_are_the_highest_round_label():
    # Consecutive failures keep `rework` on the CR, so it is added only once; round:N counts.
    events = [ev("10:00:00", "labeled", OWNER, "ready"),
              ev("10:10:00", "labeled", BOT, "rework"),
              ev("10:10:00", "labeled", BOT, "round:1"),
              ev("10:20:00", "unlabeled", BOT, "round:1"),
              ev("10:20:00", "labeled", BOT, "round:2"),
              ev("10:30:00", "closed", OWNER)]
    assert cr_numbers.numbers(events, OWNER)["rework_rounds"] == 2


def test_the_merge_time_given_wins_over_the_closed_event():
    events = [ev("10:00:00", "labeled", OWNER, "ready"), ev("11:00:00", "closed", OWNER)]
    n = cr_numbers.numbers(events, OWNER, merged_at="2026-11-13T10:45:00Z")
    assert n["ready_to_merge_min"] == 45


@pytest.mark.parametrize("events", [
    [ev("11:00:00", "closed", OWNER)],                                   # never ready by the owner
    [ev("10:00:00", "labeled", OWNER, "ready")],                         # not merged yet
])
def test_no_time_without_both_ends(events):
    assert cr_numbers.numbers(events, OWNER)["ready_to_merge_min"] is None


@pytest.mark.parametrize("minutes, text", [(0, "0m"), (51, "51m"), (120, "2h00m"),
                                           (1505, "25h05m"), (None, "n/a")])
def test_duration_text(minutes, text):
    assert cr_numbers.duration(minutes) == text


def test_line_for_the_status_issue():
    line = cr_numbers.line({"rework_rounds": 0, "ready_to_merge_min": 120, "referee_calls": 5})
    assert line == "numbers: rework rounds 0, ready to merge 2h00m, referee calls 5"


# --- the report for the blog -------------------------------------------------------------

class Gh:
    """Fake `gh api`: closed `done` issues, and one timeline per issue."""

    def __init__(self, issues, timelines):
        self.issues, self.timelines, self.calls = issues, timelines, []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        url = args[-1]
        if "/timeline" in url:
            return json.dumps(self.timelines[int(url.split("/")[-2])])
        assert "state=closed" in url and "labels=done" in url
        return json.dumps(self.issues)


def issue(number, title, milestone=None, pr=False):
    i = {"number": number, "title": title,
         "milestone": {"title": milestone} if milestone else None}
    if pr:
        i["pull_request"] = {}
    return i


def test_report_is_a_table_of_done_crs(capsys):
    gh = Gh([issue(37, "log tag", "Pipeline"), issue(43, "trace drift", "Pipeline")],
            {37: CR37, 43: CR43})
    assert cr_numbers.main(["--owner", OWNER], gh=gh) == 0
    out = capsys.readouterr().out
    assert "| CR | Rework rounds | Ready to merge | Referee calls |" in out
    assert "| #37 log tag | 1 | 19m | 0 |" in out
    assert "| #43 trace drift | 0 | 2h00m | 5 |" in out


def test_report_skips_pull_requests_and_other_milestones(capsys):
    gh = Gh([issue(43, "trace drift", "Pipeline"), issue(44, "a PR", "Pipeline", pr=True),
             issue(50, "comment fix", "M6")], {43: CR43})
    cr_numbers.main(["--owner", OWNER, "--milestone", "Pipeline"], gh=gh)
    out = capsys.readouterr().out
    assert "#43" in out and "#44" not in out and "#50" not in out


def test_report_as_json(capsys):
    gh = Gh([issue(43, "trace drift")], {43: CR43})
    cr_numbers.main(["--owner", OWNER, "--json"], gh=gh)
    assert json.loads(capsys.readouterr().out) == [
        {"issue": 43, "title": "trace drift", "rework_rounds": 0,
         "ready_to_merge_min": 120, "referee_calls": 5}]
