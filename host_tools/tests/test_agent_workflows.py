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


@pytest.mark.parametrize("name", ["builder.yml", "inspector.yml", "planner.yml", "challenger.yml"])
def test_the_action_is_pinned_to_a_commit(name):
    assert action_steps(name), f"{name}: claude-code-action not pinned to a 40-hex commit"
