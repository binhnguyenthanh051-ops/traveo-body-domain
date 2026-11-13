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


@pytest.mark.parametrize("name", ["builder.yml", "inspector.yml", "planner.yml", "challenger.yml"])
def test_the_action_is_pinned_to_a_commit(name):
    assert action_steps(name), f"{name}: claude-code-action not pinned to a 40-hex commit"
