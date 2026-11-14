"""Tests for the security-check workflow: the negative tests of the security runbook,
section 5 (owner's review of #52, point 4).

Each "refused" check passes only when GitHub refuses the action. Each has a positive control
next to it (the same identity doing something it may do), so a refusal for an unrelated
reason (a bad token, no network) cannot pass as a result. The deploy-key check is the one tag
push that must succeed, behind the owner's approval of the `release` environment."""
import pathlib
import re

import pytest

WF = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows" / "security-check.yml"


def text():
    return WF.read_text(encoding="utf-8").replace("\r\n", "\n")


def jobs():
    t = text()
    t = t[t.index("\njobs:\n"):]
    return {m.group(1): m.group(2) for m in
            re.finditer(r"^  ([a-z][a-z0-9-]*):\n(.*?)(?=^  \S|\Z)", t, re.M | re.S)}


def test_it_runs_only_by_hand():
    on = text().split("\non:\n", 1)[1].split("\n\n", 1)[0]
    assert on.strip() == "workflow_dispatch:"
    assert "permissions: {}" in text()


@pytest.mark.parametrize("job", ["app-refused", "egress-refused", "deploy-key-tag"])
def test_every_job_runs_from_main_only_and_blocks_egress(job):
    block = jobs()[job]
    assert "if: github.ref == 'refs/heads/main'" in block
    first = re.search(r"steps:\n      - (.*?)(?=\n      - |\Z)", block, re.S).group(1)
    assert first.startswith("uses: step-security/harden-runner@")
    assert "egress-policy: block" in first and "allowed-endpoints: >" in first


# @test ADR-0026 D4 : GitHub refuses the Builder app main, tags and workflows (run by hand)
def test_the_builder_app_is_refused_main_a_tag_and_a_workflow_change():
    block = jobs()["app-refused"]
    assert "private-key: ${{ secrets.BUILDER_APP_PRIVATE_KEY }}" in block
    assert 'refused "Builder app pushes to main"' in block
    assert 'refused "Builder app creates a tag"' in block
    assert 'refused "Builder app pushes a workflow change"' in block
    # the control: the same token may push an ordinary branch
    assert 'allowed "Builder app pushes an ordinary branch (control)"' in block


def test_egress_to_an_unlisted_host_is_refused():
    block = jobs()["egress-refused"]
    assert 'refused "a host not on the allowlist (example.com)"' in block
    assert 'allowed "a host on the allowlist (github.com, control)"' in block


def test_the_deploy_key_may_tag_but_not_push_main():
    block = jobs()["deploy-key-tag"]
    assert "environment: release" in block
    assert "secrets.RELEASE_DEPLOY_KEY" in block
    assert 'allowed "deploy key creates a tag"' in block
    assert 'allowed "deploy key deletes that tag"' in block
    assert 'refused "deploy key pushes to main"' in block


def test_a_check_that_gets_through_fails_the_job():
    t = text()
    # one definition of each helper, shared by every job through a file the job writes
    assert t.count("refused() {") == 1 and t.count("allowed() {") == 1
    assert "exit $fail" in t
