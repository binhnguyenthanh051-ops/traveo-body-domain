"""Tests for the agent workflows' claude-code-action steps (ADR-0026).

claude-code-action refuses a run whose triggering actor is a bot. In this pipeline the
Dispatcher reacts to CI runs that the Builder app's push started, so the Builder and the
Inspector must allow exactly that one app: never '*', which would let any bot start an agent.
Found on 2026-11-13: the first Inspector run failed with "Workflow initiated by non-human
actor: builder-github-app"."""
import pathlib
import re

import pytest

WORKFLOWS = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows"
ACTION = "anthropics/claude-code-action@"


def action_steps(name):
    text = (WORKFLOWS / name).read_text(encoding="utf-8")
    # Each step that uses the action, up to the next step.
    return [m.group(0) for m in re.finditer(
        r"uses: " + re.escape(ACTION) + r"[0-9a-f]{40}.*?(?=\n      - |\Z)", text, re.S)]


@pytest.mark.parametrize("name", ["builder.yml", "inspector.yml", "planner.yml", "challenger.yml"])
def test_the_agent_step_allows_only_the_builder_app(name):
    (step,) = action_steps(name)
    allowed = re.search(r"allowed_bots:\s*['\"]?([^'\"\n]+)", step)
    assert allowed, "allowed_bots missing"
    assert allowed.group(1).strip() == "builder-github-app"


def job_needs(job):
    """The `needs:` of one top-level job in dispatcher.yml, as a list."""
    text = (WORKFLOWS / "dispatcher.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
    text = text[text.index("\njobs:\n"):]      # `status:` is also a trigger name under `on:`
    block = re.search(r"^  " + re.escape(job) + r":\n(.*?)(?=^  \S|\Z)", text, re.M | re.S)
    assert block, f"no job {job}"
    needs = re.search(r"^    needs: (.+)$", block.group(1), re.M).group(1).strip()
    return [n.strip() for n in needs.strip("[]").split(",")]


def test_the_design_challenge_starts_right_after_route():
    # Row 18 is decided by route alone: it sits beside the other agents, not behind settle.
    assert job_needs("challenger-design") == ["route"]


def test_the_breakdown_check_follows_settle():
    # Row 19 starts only once settle has routed the Planner's result.
    assert job_needs("challenger-breakdown") == ["route", "settle"]


def test_status_waits_for_both_challenger_jobs():
    needs = job_needs("status")
    assert "challenger-design" in needs and "challenger-breakdown" in needs


def dispatcher_concurrency():
    """The workflow-level `concurrency:` block of dispatcher.yml (before `jobs:`)."""
    text = (WORKFLOWS / "dispatcher.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
    head = text[:text.index("\njobs:\n")]
    block = re.search(r"^concurrency:\n((?:  .*\n)+)", head, re.M)
    assert block, "no workflow-level concurrency in dispatcher.yml"
    return block.group(1)


def test_ticks_run_one_at_a_time_so_two_cannot_pick_the_same_cr():
    # Challenger finding on #52: a nightly tick and a button press at the same moment could
    # both see the same `ready` CR. Only ticks pick, so only ticks share one group.
    block = dispatcher_concurrency()
    group = re.search(r"group: (.+)", block).group(1)
    assert "github.event_name == 'schedule'" in group
    assert "github.event_name == 'workflow_dispatch'" in group
    assert "'dispatcher-tick'" in group


def test_every_other_event_gets_its_own_group_so_none_is_dropped():
    # GitHub keeps one pending run per group: a shared group would drop label and CI events.
    group = re.search(r"group: (.+)", dispatcher_concurrency()).group(1)
    assert "github.run_id" in group


def test_a_waiting_tick_never_cancels_the_running_one():
    assert re.search(r"cancel-in-progress: false", dispatcher_concurrency())


def job_text(job):
    text = (WORKFLOWS / "dispatcher.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
    text = text[text.index("\njobs:\n"):]
    return re.search(r"^  " + re.escape(job) + r":\n(.*?)(?=^  \S|\Z)", text, re.M | re.S).group(1)


def test_row7_an_inspector_changes_verdict_starts_the_builder_rework():
    # Found live on #50 (2026-11-14): settle decided `start: rework`, but no job read it,
    # so the CR stalled in `rework`. Row 7 is decided by settle, like row 19.
    assert job_needs("builder-rework") == ["route", "settle"]
    block = job_text("builder-rework")
    assert "needs.settle.outputs.start == 'rework'" in block
    assert "uses: ./.github/workflows/builder.yml" in block
    assert "mode: rework" in block


def test_row10_the_rework_builders_result_is_routed_too():
    # A blocked or failed rework goes to the owner (row 10), like a first build.
    assert job_needs("settle-rework") == ["route", "builder-rework"]
    block = job_text("settle-rework")
    assert "needs.builder-rework.result != 'skipped'" in block
    assert "dispatcher.py settle" in block and "--builder" in block


def test_status_reports_the_rework_round():
    needs = job_needs("status")
    assert "builder-rework" in needs and "settle-rework" in needs
    assert "needs.settle-rework.outputs.line" in job_text("status")


def test_every_dispatcher_call_reads_the_pause_switch():
    # Build step 10: PIPELINE_PAUSED stops routing in both route and settle, so a job that
    # was already running when the owner paused changes no label when it finishes.
    text = (WORKFLOWS / "dispatcher.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
    joined = text.replace("\\\n", " ")       # one shell command per line
    calls = re.findall(r"python host_tools/dispatcher\.py (?:decide|settle)[^\n]*", joined)
    assert len(calls) == 3                     # route, settle, settle-rework
    for call in calls:
        assert "$PAUSED" in call
    assert text.count("PAUSED: ${{ vars.PIPELINE_PAUSED == 'true' && '--paused' || '' }}") == 3


@pytest.mark.parametrize("name", ["builder.yml", "inspector.yml", "planner.yml", "challenger.yml"])
def test_the_action_is_pinned_to_a_commit(name):
    assert action_steps(name), f"{name}: claude-code-action not pinned to a 40-hex commit"
