"""Tests for the agent jobs' egress policy (docs/design/agent-pipeline.md section 13).

Every job in the agent workflows starts with harden-runner in `block` mode, with an explicit
allowlist. The allowlists live in one reviewable file, `.github/pipeline/egress.json`: each
host with the reason it is needed, and each job with the hosts it may reach. These tests keep
the workflows and that file in step, so a new job, a new host or a quiet switch back to
`audit` fails CI instead of passing review unseen.

The hosts were measured from StepSecurity's insights of audit-mode runs on 2026-11-13
(Challenger finding on #52: "egress audit only, separation rule open until block")."""
import json
import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
EGRESS = ROOT / ".github" / "pipeline" / "egress.json"
AGENT_WORKFLOWS = ["dispatcher.yml", "builder.yml", "inspector.yml", "planner.yml",
                   "challenger.yml"]
HARDEN = "step-security/harden-runner@"
TELEMETRY_OFF = 'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC: "1"'


def jobs(name):
    """{job name: job text} for the top-level jobs of one workflow."""
    text = (WORKFLOWS / name).read_text(encoding="utf-8").replace("\r\n", "\n")
    text = text[text.index("\njobs:\n"):]     # `status:` is also a trigger name under `on:`
    return {m.group(1): m.group(2) for m in
            re.finditer(r"^  ([a-z][a-z0-9-]*):\n(.*?)(?=^  \S|\Z)", text, re.M | re.S)}


def steps(job_text):
    return re.findall(r"^      - (.*?)(?=^      - |\Z)", job_text, re.M | re.S)


def runner_jobs():
    """{"<workflow> <job>": job text} for every job that runs on a runner (not a call)."""
    return {f"{name} {job}": text for name in AGENT_WORKFLOWS
            for job, text in jobs(name).items() if "runs-on:" in text}


def policy(step):
    mode = re.search(r"egress-policy: (\S+)", step)
    allowed = re.search(r"allowed-endpoints: >\n((?:            \S.*\n?)+)", step)
    hosts = allowed.group(1).split() if allowed else []
    return (mode.group(1) if mode else None), hosts


@pytest.fixture(scope="module")
def egress():
    return json.loads(EGRESS.read_text(encoding="utf-8"))


def test_the_agent_workflows_have_runner_jobs():
    # Guard for the parser: 12 jobs in five workflows (settle-rework added 2026-11-14).
    assert len(runner_jobs()) == 12


# @test ADR-0026 D7 : every agent job blocks egress except its measured hosts
@pytest.mark.parametrize("key", sorted(runner_jobs()))
def test_every_job_starts_with_harden_runner_in_block_mode(key):
    first = steps(runner_jobs()[key])[0]
    assert first.startswith("uses: " + HARDEN), f"{key}: harden-runner is not the first step"
    mode, hosts = policy(first)
    assert mode == "block", f"{key}: egress-policy is {mode}"
    assert hosts, f"{key}: no allowed-endpoints"


@pytest.mark.parametrize("key", sorted(runner_jobs()))
def test_every_job_allows_exactly_its_hosts_in_egress_json(key, egress):
    _, hosts = policy(steps(runner_jobs()[key])[0])
    assert key in egress["jobs"], f"{key}: no entry in egress.json"
    assert sorted(hosts) == sorted(egress["jobs"][key])


def test_egress_json_lists_no_job_that_does_not_exist(egress):
    assert sorted(egress["jobs"]) == sorted(runner_jobs())


def test_every_host_has_a_reason_and_is_used(egress):
    used = {h for hosts in egress["jobs"].values() for h in hosts}
    assert used == set(egress["hosts"])
    for host, reason in egress["hosts"].items():
        assert re.fullmatch(r"[a-z0-9.-]+\.[a-z]+:\d+", host), f"{host}: not host:port"
        assert len(reason) > 10, f"{host}: no reason"


def test_no_job_allows_telemetry(egress):
    assert not any("datadoghq" in h or "sentry" in h for h in egress["hosts"])


@pytest.mark.parametrize("key", sorted(k for k, t in runner_jobs().items()
                                       if "anthropics/claude-code-action@" in t))
def test_every_agent_job_turns_off_nonessential_traffic_and_scrubs_subprocesses(key):
    # Job-level env: the action reads these through the `env` context and its docs say a
    # composite step's own env block can shadow a step-level value (owner's review of #52, 2).
    # - telemetry: Claude Code sends it (Datadog was measured); off, rather than allowed.
    # - scrub: without it, `make test` and any code the Builder writes can read the OAuth token.
    job_env = runner_jobs()[key].split("    steps:", 1)[0]
    assert TELEMETRY_OFF in job_env
    assert 'CLAUDE_CODE_SUBPROCESS_ENV_SCRUB: "1"' in job_env


# --- sudo and containers (ADR-0026 D7) -----------------------------------------------------
# With sudo or Docker, code run in the job (a test the Builder writes, then runs with
# `make test`) could flush harden-runner's firewall rules, so the block would hold only on paper.

NEEDS_SUDO = {"builder.yml build"}   # apt-get install cppcheck; drops sudo itself afterwards


@pytest.mark.parametrize("key", sorted(set(runner_jobs()) - NEEDS_SUDO))
def test_every_other_job_disables_sudo_and_containers(key):
    first = steps(runner_jobs()[key])[0]
    assert re.search(r"disable-sudo-and-containers: ['\"]?true", first), key


def test_the_builder_drops_sudo_and_docker_right_after_its_tools():
    names = [re.match(r"(?:name: )?(.*)", s).group(1).splitlines()[0] for s in
             steps(runner_jobs()["builder.yml build"])]
    tools = next(i for i, n in enumerate(names) if n.startswith("Tools"))
    drop = steps(runner_jobs()["builder.yml build"])[tools + 1]
    assert "sudo truncate -s 0 /etc/sudoers.d/runner" in drop
    assert "/var/run/docker.sock" in drop and "/run/containerd/containerd.sock" in drop
    # and it proves it: the step fails if sudo or Docker still work afterwards
    assert "sudo -n true" in drop and "docker info" in drop


def test_no_step_after_the_drop_uses_sudo():
    s = steps(runner_jobs()["builder.yml build"])
    drop = next(i for i, step in enumerate(s) if "/etc/sudoers.d/runner" in step)
    assert not any(re.search(r"\bsudo\b(?! -n true)", step) for step in s[drop + 1:])
