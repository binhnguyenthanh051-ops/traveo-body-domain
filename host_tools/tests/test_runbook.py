"""Tests for the agent pipeline's security runbook (docs/security/agent-pipeline-runbook.md).

The Challenger on #52 asked for a rotation and revocation procedure for every credential and
an incident runbook beyond the pause switch. These tests keep the runbook complete: every
secret a workflow reads, plus the two credentials outside GitHub secrets (the release deploy
key and the bench token), has a row in its rotation table."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNBOOK = ROOT / "docs" / "security" / "agent-pipeline-runbook.md"
WORKFLOWS = ROOT / ".github" / "workflows"
OUTSIDE_SECRETS = {"RELEASE_DEPLOY_KEY", "bench token"}


def workflow_secrets():
    names = set()
    for path in WORKFLOWS.glob("*.yml"):
        names |= set(re.findall(r"secrets\.([A-Z][A-Z0-9_]+)", path.read_text(encoding="utf-8")))
    return names - {"GITHUB_TOKEN"}


def rotation_rows():
    text = RUNBOOK.read_text(encoding="utf-8")
    section = text[text.index("## 1. Rotate"):text.index("## 2.")]
    return [line for line in section.splitlines() if line.startswith("| `")
            or line.startswith("| bench token")]


def test_the_workflows_read_the_secrets_we_know():
    # Guard for the parser, 2026-11-14.
    assert workflow_secrets() == {"CLAUDE_CODE_OAUTH_TOKEN", "BUILDER_APP_PRIVATE_KEY",
                                  "REVIEWER_APP_PRIVATE_KEY", "RELEASE_DEPLOY_KEY"}


def test_every_credential_has_a_rotation_row():
    rows = "\n".join(rotation_rows())
    for name in workflow_secrets() | OUTSIDE_SECRETS:
        assert name in rows, f"{name}: no row in the rotation table"


def test_the_runbook_has_the_incident_and_schedule_sections():
    text = RUNBOOK.read_text(encoding="utf-8")
    for heading in ("## 1. Rotate", "## 2. Incident", "## 3. The nightly schedule",
                    "## 4. The subscription token"):
        assert heading in text, heading
    # The first move in an incident is the one that stops everything.
    incident = text[text.index("## 2. Incident"):text.index("## 3.")]
    assert incident.index("PIPELINE_PAUSED") < incident.index("Suspend")
