"""Tests for pipeline_setup -- the agent pipeline's labels, milestones and issue forms.

The contract comes from docs/design/agent-pipeline.md: section 4 names the nine state
labels (label = board column) and the fields of the two issue forms, section 7 names the
label that lets a pull request touch a protected path. These tests pin that contract
against the files in .github/, and check that the setup tool only ever adds or updates:
it never deletes, and it changes nothing without --apply.

No test talks to GitHub: the tool takes its `gh` runner as an argument, and the tests
pass a fake that records the calls."""
import json
import pathlib
import re
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import pipeline_setup  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
FORMS = REPO / ".github" / "ISSUE_TEMPLATE"

# Design note section 4, "States and labels", in board order.
STATE_LABELS = ["draft", "ready", "building", "review", "rework",
                "needs-bench", "awaiting-merge", "waiting-for-me", "done"]

# Design note section 4, "Issue forms": field id -> the heading GitHub renders.
CR_FIELDS = {
    "type": "Type",
    "what-and-why": "What and why",
    "adr-link": "ADR link",
    "requirement-ids": "Requirement IDs",
    "acceptance-criteria": "Acceptance criteria",
    "test-level": "Test level",
    "milestone": "Milestone",
}
BUG_FIELDS = {
    "observed": "Observed",
    "expected": "Expected",
    "how-to-reproduce": "How to reproduce",
    "affected-requirement": "Affected requirement",
    "test-level": "Test level",
    "milestone": "Milestone",
}


def real_config():
    return pipeline_setup.load_config(pipeline_setup.DEFAULT_CONFIG)


def form_fields(text):
    """id -> label for every input field of an issue form."""
    return dict(re.findall(r"^\s+id: (\S+)\n\s+attributes:\n\s+label: (.+)$", text, re.M))


def form_labels(text):
    return json.loads(re.search(r"^labels: (\[.*\])$", text, re.M).group(1))


def label(name, color="aaaaaa", description="d", kind="state"):
    return {"name": name, "kind": kind, "color": color, "description": description}


# --- the contract: the real files match the design note -------------------------------

def test_real_config_is_valid():
    assert pipeline_setup.validate(real_config()) == []


def test_state_labels_are_the_nine_board_columns_in_order():
    states = [l["name"] for l in real_config()["labels"] if l["kind"] == "state"]
    assert states == STATE_LABELS


def test_protected_change_label_is_defined():
    names = {l["name"] for l in real_config()["labels"]}
    assert "protected-change-approved" in names


def test_level_and_round_labels_are_defined_for_the_dispatcher():
    # The Dispatcher reads labels only: the test level and the rework round are labels.
    control = {l["name"] for l in real_config()["labels"] if l["kind"] == "control"}
    assert {"level:host", "level:on-target", "level:both",
            "round:1", "round:2", "round:3"} <= control


def test_planner_labels_are_defined():
    names = {l["name"] for l in real_config()["labels"]}
    assert {"plan:breakdown", "plan:bug", "planning", "planned"} <= names


def test_every_cr_belongs_to_a_milestone_that_exists():
    titles = [m["title"] for m in real_config()["milestones"]]
    assert titles, "section 4: each CR belongs to one GitHub milestone"
    assert len(titles) == len(set(titles))


@pytest.mark.parametrize("form, fields, labels", [
    ("change-request.yml", CR_FIELDS, ["draft", "cr"]),
    ("bug-report.yml", BUG_FIELDS, ["draft", "bug"]),
])
def test_form_has_the_fields_from_the_design_note(form, fields, labels):
    text = (FORMS / form).read_text(encoding="utf-8")
    assert form_fields(text) == fields
    # "Agent-ready" means every field is filled, so every field is required.
    assert text.count("required: true") == len(fields)
    assert form_labels(text) == labels


@pytest.mark.parametrize("form", ["change-request.yml", "bug-report.yml"])
def test_form_starts_as_draft_and_uses_only_defined_labels(form):
    text = (FORMS / form).read_text(encoding="utf-8")
    defined = {l["name"] for l in real_config()["labels"]}
    assert form_labels(text)[0] == "draft"   # only the owner sets ready
    assert set(form_labels(text)) <= defined


@pytest.mark.parametrize("form", ["change-request.yml", "bug-report.yml"])
def test_form_offers_the_three_test_levels(form):
    text = (FORMS / form).read_text(encoding="utf-8")
    block = text.split("id: test-level")[1].split("validations:")[0]
    assert re.findall(r"^\s+- (\S+)$", block, re.M) == ["host", "on-target", "both"]


# --- validate ---------------------------------------------------------------------------

def test_validate_rejects_a_duplicate_label():
    config = {"labels": [label("draft"), label("draft")], "milestones": []}
    assert any("duplicate" in e for e in pipeline_setup.validate(config))


@pytest.mark.parametrize("color", ["#c5def5", "c5def", "zzzzzz", ""])
def test_validate_rejects_a_bad_color(color):
    config = {"labels": [label("draft", color=color)], "milestones": []}
    assert any("color" in e for e in pipeline_setup.validate(config))


def test_validate_rejects_a_description_github_would_refuse():
    config = {"labels": [label("draft", description="x" * 101)], "milestones": []}
    assert any("description" in e for e in pipeline_setup.validate(config))


def test_validate_rejects_an_unknown_kind():
    config = {"labels": [label("draft", kind="mood")], "milestones": []}
    assert any("kind" in e for e in pipeline_setup.validate(config))


@pytest.mark.parametrize("due", ["15-11-2026", "2026-11-15T08:00:00Z", "soon"])
def test_validate_rejects_a_bad_due_date(due):
    config = {"labels": [], "milestones": [{"title": "M6", "description": "", "due_on": due}]}
    assert any("due_on" in e for e in pipeline_setup.validate(config))


def test_validate_rejects_a_duplicate_milestone():
    m = {"title": "M6", "description": "", "due_on": None}
    assert any("duplicate" in e
               for e in pipeline_setup.validate({"labels": [], "milestones": [m, m]}))


# --- plan: what would change ------------------------------------------------------------

def ops(actions):
    return [(a.kind, a.op, a.name) for a in actions]


def test_plan_creates_a_missing_label():
    assert ops(pipeline_setup.plan_labels([label("draft")], [])) == [("label", "create", "draft")]


def test_plan_keeps_a_label_that_already_matches():
    existing = [{"name": "draft", "color": "AAAAAA", "description": "d"}]   # GitHub's case
    assert ops(pipeline_setup.plan_labels([label("draft")], existing)) == [("label", "keep", "draft")]


@pytest.mark.parametrize("existing", [
    {"name": "draft", "color": "bbbbbb", "description": "d"},
    {"name": "draft", "color": "aaaaaa", "description": "old words"},
])
def test_plan_updates_a_label_that_differs(existing):
    assert ops(pipeline_setup.plan_labels([label("draft")], [existing])) == [("label", "update", "draft")]


def test_plan_never_touches_a_label_it_does_not_manage():
    existing = [{"name": "wontfix", "color": "ffffff", "description": "x"}]
    assert ops(pipeline_setup.plan_labels([label("draft")], existing)) == [("label", "create", "draft")]


def test_plan_creates_a_missing_milestone():
    wanted = [{"title": "M6", "description": "d", "due_on": None}]
    assert ops(pipeline_setup.plan_milestones(wanted, [])) == [("milestone", "create", "M6")]


def test_plan_keeps_a_milestone_and_compares_the_due_date_by_day():
    wanted = [{"title": "Pipeline", "description": "d", "due_on": "2026-11-15"}]
    existing = [{"number": 3, "title": "Pipeline", "description": "d",
                 "due_on": "2026-11-15T08:00:00Z"}]
    assert ops(pipeline_setup.plan_milestones(wanted, existing)) == [("milestone", "keep", "Pipeline")]


def test_plan_updates_a_milestone_by_its_number():
    wanted = [{"title": "Pipeline", "description": "new", "due_on": "2026-11-15"}]
    existing = [{"number": 3, "title": "Pipeline", "description": "old",
                 "due_on": "2026-11-15T08:00:00Z"}]
    (action,) = pipeline_setup.plan_milestones(wanted, existing)
    assert (action.op, action.number) == ("update", 3)


def test_plan_leaves_the_due_date_alone_when_the_config_sets_none():
    wanted = [{"title": "M6", "description": "d", "due_on": None}]
    existing = [{"number": 1, "title": "M6", "description": "d", "due_on": "2027-01-31T08:00:00Z"}]
    assert ops(pipeline_setup.plan_milestones(wanted, existing)) == [("milestone", "keep", "M6")]


# --- ruleset: the required checks on main (build step 2) ---------------------------------

# Design note section 5 and build step 2: tests, traceability and protected paths.
REQUIRED_CHECKS = ["host-build-test", "static-analysis", "python-tools",
                   "traceability", "protected-paths"]


def ruleset(checks, name="Protect_main_branch", rid=7):
    """A ruleset as GET /rulesets/{id} returns it, with the rules the owner set up."""
    return {"id": rid, "name": name, "enforcement": "active", "rules": [
        {"type": "deletion", "parameters": None},
        {"type": "non_fast_forward", "parameters": None},
        {"type": "required_status_checks", "parameters": {
            "strict_required_status_checks_policy": True,
            "required_status_checks": [{"context": c, "integration_id": 15368} for c in checks]}},
        {"type": "required_linear_history", "parameters": None},
    ]}


def test_real_config_requires_the_three_kinds_of_check():
    assert real_config()["ruleset"]["required_checks"] == REQUIRED_CHECKS


def test_every_required_check_is_a_job_in_a_pull_request_workflow():
    jobs = set()
    for wf in (REPO / ".github" / "workflows").glob("*.yml"):
        text = wf.read_text(encoding="utf-8")
        if "pull_request" in text:
            jobs |= set(re.findall(r"^  ([a-z][a-z0-9-]*):\s*$", text, re.M))
    assert set(real_config()["ruleset"]["required_checks"]) <= jobs


def test_plan_keeps_a_ruleset_with_the_same_checks_in_any_order():
    action = pipeline_setup.plan_ruleset(
        {"name": "Protect_main_branch", "required_checks": ["b", "a"]}, ruleset(["a", "b"]))
    assert (action.op, action.number) == ("keep", 7)


def test_plan_updates_a_ruleset_with_other_checks():
    action = pipeline_setup.plan_ruleset(
        {"name": "Protect_main_branch", "required_checks": ["a", "b"]}, ruleset(["a"]))
    assert (action.op, action.number) == ("update", 7)


def test_plan_reports_a_missing_ruleset_and_never_creates_one():
    action = pipeline_setup.plan_ruleset({"name": "Protect_main_branch", "required_checks": []}, None)
    assert action.op == "missing"


def test_new_rules_change_only_the_required_checks():
    before = ruleset(["a"])
    after = pipeline_setup.ruleset_rules(before, ["a", "b"])
    checks = next(r for r in after if r["type"] == "required_status_checks")["parameters"]
    assert [c["context"] for c in checks["required_status_checks"]] == ["a", "b"]
    assert all(c["integration_id"] == 15368 for c in checks["required_status_checks"])
    assert checks["strict_required_status_checks_policy"] is True
    others = [r for r in after if r["type"] != "required_status_checks"]
    assert others == [r for r in before["rules"] if r["type"] != "required_status_checks"]


# --- main: dry run by default, writes only with --apply ----------------------------------

class FakeGh:
    """Stands in for the `gh` CLI: answers the reads, records every call and its stdin."""

    def __init__(self, labels=(), milestones=(), rulesets=(), paths=None):
        self.labels, self.milestones, self.calls = list(labels), list(milestones), []
        self.rulesets = {r["id"]: r for r in rulesets}
        self.paths = {"repos/{owner}/{repo}": {"owner": {"login": "owner"}},
                      "users/owner": {"id": 42}, "repos/{owner}/{repo}/keys": []}
        self.paths.update(paths or {})
        self.stdin = []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        self.stdin.append(stdin)
        if args[:2] == ["label", "list"]:
            return json.dumps(self.labels)
        if args[0] == "api" and "--method" not in args:
            path = args[1]
            if path in self.paths:
                if self.paths[path] is None:      # GitHub answers 404
                    raise subprocess.CalledProcessError(1, "gh", stderr="Not Found (HTTP 404)")
                return json.dumps(self.paths[path])
            if "/environments/" in path:
                raise subprocess.CalledProcessError(1, "gh", stderr="Not Found (HTTP 404)")
            if "/rulesets/" in path:
                return json.dumps(self.rulesets[int(path.rsplit("/", 1)[1])])
            if "/rulesets" in path:
                return json.dumps([{"id": i, "name": r["name"]} for i, r in self.rulesets.items()])
            return json.dumps(self.milestones)
        return ""

    def writes(self):
        return [c for c in self.calls if c[:2] == ["label", "create"] or "--method" in c]


def write_config(tmp_path, checks=("a",)):
    path = tmp_path / "setup.json"
    path.write_text(json.dumps({
        "labels": [label("draft"), label("ready")],
        "milestones": [{"title": "M6", "description": "d", "due_on": "2026-11-15"}],
        "ruleset": {"name": "Protect_main_branch", "required_checks": list(checks)},
    }), encoding="utf-8")
    return path


def test_apply_puts_the_new_checks_on_the_existing_ruleset(tmp_path):
    gh = FakeGh(rulesets=[ruleset(["a"])])
    config = write_config(tmp_path, checks=("a", "b"))
    assert pipeline_setup.main(["--config", str(config), "--apply"], gh=gh) == 0
    (put,) = [c for c in gh.writes() if "PUT" in c]
    assert put[-1].endswith("/rulesets/7")
    body = json.loads(gh.stdin[gh.calls.index(put)])
    checks = next(r for r in body["rules"] if r["type"] == "required_status_checks")
    assert [c["context"] for c in checks["parameters"]["required_status_checks"]] == ["a", "b"]


def test_a_missing_ruleset_stops_with_an_error(tmp_path, capsys):
    gh = FakeGh(rulesets=[ruleset(["a"], name="Something_else")])
    assert pipeline_setup.main(["--config", str(write_config(tmp_path)), "--apply"], gh=gh) == 1
    assert gh.writes() == []
    assert "Protect_main_branch" in capsys.readouterr().out


def test_dry_run_reads_github_and_changes_nothing(tmp_path, capsys):
    gh = FakeGh(rulesets=[ruleset(["a"])])
    assert pipeline_setup.main(["--config", str(write_config(tmp_path))], gh=gh) == 0
    assert gh.writes() == []
    out = capsys.readouterr().out
    assert "3 change(s) pending" in out and "--apply" in out


def test_apply_makes_exactly_the_planned_changes(tmp_path):
    gh = FakeGh(labels=[{"name": "draft", "color": "aaaaaa", "description": "d"}],
                rulesets=[ruleset(["a"])])
    assert pipeline_setup.main(["--config", str(write_config(tmp_path)), "--apply"], gh=gh) == 0
    writes = gh.writes()
    assert len(writes) == 2                                   # draft is kept
    assert writes[0][:3] == ["label", "create", "ready"]
    assert "title=M6" in writes[1] and "due_on=2026-11-15T12:00:00Z" in writes[1]


def test_apply_is_idempotent(tmp_path, capsys):
    gh = FakeGh(
        labels=[{"name": n, "color": "aaaaaa", "description": "d"} for n in ("draft", "ready")],
        milestones=[{"number": 1, "title": "M6", "description": "d",
                     "due_on": "2026-11-15T08:00:00Z"}],
        rulesets=[ruleset(["a"])],
    )
    assert pipeline_setup.main(["--config", str(write_config(tmp_path)), "--apply"], gh=gh) == 0
    assert gh.writes() == []
    assert "nothing to do" in capsys.readouterr().out


# --- managed rulesets: owner approval and tags (ADR-0026, build step 4) -----------------

ADMIN = {"actor_id": 5, "actor_type": "RepositoryRole"}
ACTIONS = {"actor_id": 15368, "actor_type": "Integration"}


def managed(name):
    return next(r for r in real_config()["managed_rulesets"] if r["name"] == name)


def test_main_needs_one_approval_and_only_the_owner_bypasses_it_through_a_pr():
    r = managed("Owner_approval")
    assert r["target"] == "branch"
    assert r["conditions"]["ref_name"]["include"] == ["~DEFAULT_BRANCH"]
    (rule,) = r["rules"]
    assert rule["type"] == "pull_request"
    assert rule["parameters"]["required_approving_review_count"] == 1
    assert r["bypass_actors"] == [dict(ADMIN, bypass_mode="pull_request")]


def test_only_the_owner_and_the_release_workflow_touch_tags():
    r = managed("Tags_owner_only")
    assert r["target"] == "tag"
    assert r["conditions"]["ref_name"]["include"] == ["~ALL"]
    assert {rule["type"] for rule in r["rules"]} == {"creation", "update", "deletion"}
    # GitHub refuses the GitHub Actions app as a bypass actor on a personal-account repo
    # (HTTP 422, 2026-11-13). release.yml pushes the tag with a deploy key instead.
    assert r["bypass_actors"] == [dict(ADMIN, bypass_mode="always"),
                                  {"actor_id": None, "actor_type": "DeployKey",
                                   "bypass_mode": "always"}]


# --- the release environment: only owner-approved runs get the deploy key ---------------

def test_release_environment_needs_the_owner_and_runs_only_from_main():
    (env,) = real_config()["environments"]
    assert env == {"name": "release", "reviewers": ["{owner}"], "branches": ["main"]}


ENV = {"name": "release", "reviewers": ["{owner}"], "branches": ["main"]}


def env_on_github(reviewer="owner", custom=True):
    return {"name": "release",
            "protection_rules": [
                {"type": "required_reviewers", "prevent_self_review": False,
                 "reviewers": [{"type": "User", "reviewer": {"login": reviewer, "id": 42}}]},
                {"type": "branch_policy"}],
            "deployment_branch_policy": {"protected_branches": not custom,
                                         "custom_branch_policies": custom}}


@pytest.mark.parametrize("existing, branches, op", [
    (None, None, "create"),
    (env_on_github(), ["main"], "keep"),
    (env_on_github(reviewer="someone"), ["main"], "update"),
    (env_on_github(), ["dev"], "update"),
    (env_on_github(custom=False), [], "update"),
])
def test_plan_environment(existing, branches, op):
    action = pipeline_setup.plan_environment(ENV, existing, branches, "owner")
    assert (action.kind, action.op, action.name) == ("environment", op, "release")


def test_apply_puts_the_environment_with_the_owner_as_reviewer(tmp_path):
    config = write_config(tmp_path)
    data = json.loads(config.read_text(encoding="utf-8"))
    data["environments"] = [ENV]
    config.write_text(json.dumps(data), encoding="utf-8")
    gh = FakeGh(rulesets=[ruleset(["a"])])
    assert pipeline_setup.main(["--config", str(config), "--apply"], gh=gh) == 0
    (put,) = [c for c in gh.writes() if "PUT" in c and "environments/release" in c[-1]]
    body = json.loads(gh.stdin[gh.calls.index(put)])
    assert body["reviewers"] == [{"type": "User", "id": 42}]
    assert body["prevent_self_review"] is False          # the owner approves their own run
    assert body["deployment_branch_policy"] == {"protected_branches": False,
                                                "custom_branch_policies": True}
    (post,) = [c for c in gh.writes() if "deployment-branch-policies" in c[-1]]
    assert json.loads(gh.stdin[gh.calls.index(post)]) == {"name": "main", "type": "branch"}


def test_more_than_one_write_deploy_key_is_an_error(tmp_path, capsys):
    # The DeployKey bypass covers every deploy key with write access, so only one may exist.
    keys = [{"id": 1, "title": "release", "read_only": False},
            {"id": 2, "title": "other", "read_only": False}]
    gh = FakeGh(rulesets=[ruleset(["a"])], paths={"repos/{owner}/{repo}/keys": keys})
    assert pipeline_setup.main(["--config", str(write_config(tmp_path)), "--apply"], gh=gh) == 1
    assert gh.writes() == []
    assert "deploy key" in capsys.readouterr().out


def test_both_managed_rulesets_are_active():
    assert {r["enforcement"] for r in real_config()["managed_rulesets"]} == {"active"}


def spec(count=1, bypass=None):
    return {"name": "Owner_approval", "target": "branch", "enforcement": "active",
            "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
            "rules": [{"type": "pull_request",
                       "parameters": {"required_approving_review_count": count}}],
            "bypass_actors": [dict(ADMIN, bypass_mode="pull_request")] if bypass is None else bypass}


def on_github(s, rid=11):
    """The spec as GitHub returns it: server-only fields added, rule parameters filled in."""
    got = json.loads(json.dumps(s))
    got.update({"id": rid, "source": "o/r", "source_type": "Repository",
                "node_id": "RRS_x", "_links": {}, "current_user_can_bypass": "always",
                "created_at": "t", "updated_at": "t"})
    for rule in got["rules"]:
        if rule["type"] == "pull_request":
            rule["parameters"].setdefault("dismiss_stale_reviews_on_push", False)
    return got


def test_plan_creates_a_missing_managed_ruleset():
    assert ops([pipeline_setup.plan_managed(spec(), None)]) == [("ruleset", "create", "Owner_approval")]


def test_plan_keeps_a_managed_ruleset_that_matches_including_github_defaults():
    action = pipeline_setup.plan_managed(spec(), on_github(spec()))
    assert (action.op, action.number) == ("keep", 11)


@pytest.mark.parametrize("changed", [spec(count=0), spec(bypass=[])])
def test_plan_updates_a_managed_ruleset_that_differs(changed):
    action = pipeline_setup.plan_managed(spec(), on_github(changed))
    assert (action.op, action.number) == ("update", 11)


def test_plan_keeps_the_real_tag_ruleset_as_github_returns_it():
    # GitHub returns the DeployKey actor with actor_id null next to the admin's 5; comparing
    # them crashed the second --apply run on 2026-11-13.
    tags = managed("Tags_owner_only")
    on = on_github(tags, rid=13)
    on["bypass_actors"] = list(reversed(on["bypass_actors"]))   # GitHub's order differs
    action = pipeline_setup.plan_managed(tags, on)
    assert (action.op, action.number) == ("keep", 13)


def test_checks_ruleset_with_a_bypass_actor_is_an_error():
    # Required checks apply to the owner too: the checks ruleset must have no bypass.
    existing = ruleset(["a"])
    existing["bypass_actors"] = [dict(ADMIN, bypass_mode="always")]
    action = pipeline_setup.plan_ruleset({"name": "Protect_main_branch", "required_checks": ["a"]},
                                         existing)
    assert action.op == "has-bypass"


def test_apply_creates_and_updates_managed_rulesets(tmp_path):
    config = write_config(tmp_path)
    data = json.loads(config.read_text(encoding="utf-8"))
    tags = dict(spec(), name="Tags_owner_only")
    data["managed_rulesets"] = [spec(), tags]
    config.write_text(json.dumps(data), encoding="utf-8")
    gh = FakeGh(rulesets=[ruleset(["a"]), on_github(dict(tags, enforcement="evaluate"), rid=12)])
    assert pipeline_setup.main(["--config", str(config), "--apply"], gh=gh) == 0
    posts = [c for c in gh.writes() if "POST" in c and c[-1].endswith("/rulesets")]
    puts = [c for c in gh.writes() if "PUT" in c and c[-1].endswith("/rulesets/12")]
    assert len(posts) == 1 and len(puts) == 1
    body = json.loads(gh.stdin[gh.calls.index(posts[0])])
    assert body["name"] == "Owner_approval" and body["bypass_actors"][0]["bypass_mode"] == "pull_request"
    assert "id" not in body


def test_an_invalid_config_stops_before_github_is_read(tmp_path, capsys):
    path = tmp_path / "setup.json"
    path.write_text(json.dumps({"labels": [label("draft", color="nope")], "milestones": []}),
                    encoding="utf-8")
    gh = FakeGh()
    assert pipeline_setup.main(["--config", str(path), "--apply"], gh=gh) == 1
    assert gh.calls == []
    assert "color" in capsys.readouterr().out
