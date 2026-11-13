#!/usr/bin/env python3
"""Apply the agent pipeline's GitHub labels, milestones and required checks on main.

Source of truth: .github/pipeline/setup.json (docs/design/agent-pipeline.md, sections 4
and 7). This tool compares that file with the repository on GitHub and prints what
differs. It changes GitHub only with --apply.

    python host_tools/pipeline_setup.py            # dry run: show the plan
    python host_tools/pipeline_setup.py --apply    # make the changes

It only creates and updates. A label or milestone that is not in the file is left alone,
and nothing is ever deleted, so a second run after --apply reports "nothing to do".
For the ruleset on main it changes only the list of required checks, and it never creates
a ruleset: the other rules (pull request, no force push, linear history) are the owner's.

It talks to GitHub through the `gh` CLI, in the repository of the current directory.
Exit code: 0 = done (or dry run), 1 = the config file is invalid, 2 = `gh` failed.
"""
import argparse
import copy
import dataclasses
import json
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = REPO_ROOT / ".github" / "pipeline" / "setup.json"

LABEL_KINDS = ("state", "type", "control")
MAX_DESCRIPTION = 100          # GitHub refuses a longer label description
MILESTONES_API = "repos/{owner}/{repo}/milestones"   # gh fills in owner and repo
RULESETS_API = "repos/{owner}/{repo}/rulesets"
GITHUB_ACTIONS_APP = 15368     # integration id of GitHub Actions, the app behind the checks


@dataclasses.dataclass(frozen=True)
class Action:
    """One line of the plan. `number` is the GitHub milestone number, for an update."""
    kind: str      # "label" | "milestone"
    op: str        # "create" | "update" | "keep"
    name: str
    number: int | None = None


def load_config(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def validate(config):
    """Return a list of problems with the config; empty when it is usable."""
    errors = []
    seen = set()
    for label in config.get("labels", []):
        name = label.get("name", "")
        if not name:
            errors.append("label without a name")
        if name in seen:
            errors.append(f"label '{name}': duplicate name")
        seen.add(name)
        if not re.fullmatch(r"[0-9a-fA-F]{6}", label.get("color", "")):
            errors.append(f"label '{name}': color must be six hex digits, without '#'")
        if len(label.get("description", "")) > MAX_DESCRIPTION:
            errors.append(f"label '{name}': description is longer than {MAX_DESCRIPTION} characters")
        if label.get("kind") not in LABEL_KINDS:
            errors.append(f"label '{name}': kind must be one of {', '.join(LABEL_KINDS)}")

    seen = set()
    for milestone in config.get("milestones", []):
        title = milestone.get("title", "")
        if not title:
            errors.append("milestone without a title")
        if title in seen:
            errors.append(f"milestone '{title}': duplicate title")
        seen.add(title)
        due = milestone.get("due_on")
        if due is not None and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", str(due)):
            errors.append(f"milestone '{title}': due_on must be YYYY-MM-DD or null")
    return errors


def plan_labels(wanted, existing):
    """Compare the wanted labels with GitHub's. Labels only GitHub has are ignored."""
    current = {label["name"]: label for label in existing}
    actions = []
    for label in wanted:
        have = current.get(label["name"])
        if have is None:
            op = "create"
        elif (have["color"].lower() != label["color"].lower()
              or (have.get("description") or "") != label["description"]):
            op = "update"
        else:
            op = "keep"
        actions.append(Action("label", op, label["name"]))
    return actions


def plan_milestones(wanted, existing):
    """Compare the wanted milestones with GitHub's, matched by title.

    A due date is compared by day, because GitHub stores it as a timestamp. When the
    config says null, the due date on GitHub is not managed and never causes an update.
    """
    current = {milestone["title"]: milestone for milestone in existing}
    actions = []
    for milestone in wanted:
        have = current.get(milestone["title"])
        if have is None:
            actions.append(Action("milestone", "create", milestone["title"]))
            continue
        same_text = (have.get("description") or "") == milestone["description"]
        due = milestone.get("due_on")
        same_due = due is None or (have.get("due_on") or "")[:10] == due
        op = "keep" if same_text and same_due else "update"
        actions.append(Action("milestone", op, milestone["title"], have["number"]))
    return actions


def plan_ruleset(wanted, existing):
    """Compare the wanted required checks with the ruleset's. `existing` is None when no
    ruleset has the wanted name: the tool reports that and never creates one, because the
    ruleset's other rules are the owner's to set."""
    if existing is None:
        return Action("ruleset", "missing", wanted["name"])
    if existing.get("bypass_actors"):
        # Required checks apply to the owner too (ADR-0026): nobody bypasses this ruleset.
        return Action("ruleset", "has-bypass", wanted["name"], existing["id"])
    have = {check["context"] for check in _checks_rule(existing["rules"])
            .get("parameters", {}).get("required_status_checks", [])}
    op = "keep" if have == set(wanted["required_checks"]) else "update"
    return Action("ruleset", op, wanted["name"], existing["id"])


MANAGED_KEYS = ("name", "target", "enforcement", "conditions", "rules", "bypass_actors")


def managed_body(spec):
    """The request body for a managed ruleset: the spec without comments."""
    return {k: spec[k] for k in MANAGED_KEYS if k in spec}


def _same_managed(spec, existing):
    """True when GitHub's ruleset matches the spec. Only what the spec states is compared,
    because GitHub fills in rule parameters the spec leaves at their defaults."""
    for key in ("name", "target", "enforcement"):
        if spec[key] != existing.get(key):
            return False
    want_ref = spec["conditions"]["ref_name"]
    have_ref = existing.get("conditions", {}).get("ref_name", {})
    for key in ("include", "exclude"):
        if sorted(want_ref.get(key, [])) != sorted(have_ref.get(key, [])):
            return False
    have_rules = {rule["type"]: rule.get("parameters") or {} for rule in existing.get("rules", [])}
    if set(have_rules) != {rule["type"] for rule in spec["rules"]}:
        return False
    for rule in spec["rules"]:
        for key, value in (rule.get("parameters") or {}).items():
            if have_rules[rule["type"]].get(key) != value:
                return False

    def actors(items):
        return sorted((a.get("actor_id"), a["actor_type"], a["bypass_mode"]) for a in items)
    return actors(spec["bypass_actors"]) == actors(existing.get("bypass_actors", []))


ENVIRONMENTS_API = "repos/{owner}/{repo}/environments"


def repo_owner(gh):
    return json.loads(gh(["api", "repos/{owner}/{repo}"]))["owner"]["login"]


def read_environment(gh, name):
    """Return (environment or None, its custom deployment branch names)."""
    try:
        env = json.loads(gh(["api", f"{ENVIRONMENTS_API}/{name}"]))
    except subprocess.CalledProcessError:
        return None, []
    if not (env.get("deployment_branch_policy") or {}).get("custom_branch_policies"):
        return env, []
    policies = json.loads(gh(["api", f"{ENVIRONMENTS_API}/{name}/deployment-branch-policies"]))
    return env, [p["name"] for p in policies.get("branch_policies", [])]


def _reviewers(spec, owner):
    return sorted(owner if r == "{owner}" else r for r in spec["reviewers"])


def plan_environment(spec, existing, branches, owner):
    """An environment whose runs need the reviewers' approval and start only from the
    listed branches (ADR-0026: only an owner-approved release run gets the deploy key)."""
    if existing is None:
        return Action("environment", "create", spec["name"])
    rule = next((r for r in existing.get("protection_rules", [])
                 if r["type"] == "required_reviewers"), {})
    have = sorted(r["reviewer"]["login"] for r in rule.get("reviewers", []))
    custom = (existing.get("deployment_branch_policy") or {}).get("custom_branch_policies")
    same = (have == _reviewers(spec, owner) and custom
            and not rule.get("prevent_self_review", False)
            and sorted(branches) == sorted(spec["branches"]))
    return Action("environment", "keep" if same else "update", spec["name"])


def write_deploy_keys(gh):
    return [k for k in json.loads(gh(["api", "repos/{owner}/{repo}/keys"])) if not k["read_only"]]


def plan_managed(spec, existing):
    """A ruleset this tool owns completely: create it, update it, or keep it."""
    if existing is None:
        return Action("ruleset", "create", spec["name"])
    op = "keep" if _same_managed(spec, existing) else "update"
    return Action("ruleset", op, spec["name"], existing["id"])


def _checks_rule(rules):
    return next((rule for rule in rules if rule["type"] == "required_status_checks"), {})


def ruleset_rules(existing, checks):
    """The ruleset's rules with only the list of required checks replaced."""
    rules = copy.deepcopy(existing["rules"])
    rule = _checks_rule(rules)
    if not rule:
        rule = {"type": "required_status_checks",
                "parameters": {"strict_required_status_checks_policy": True}}
        rules.append(rule)
    old = rule["parameters"].get("required_status_checks", [])
    app = old[0].get("integration_id", GITHUB_ACTIONS_APP) if old else GITHUB_ACTIONS_APP
    rule["parameters"]["required_status_checks"] = [
        {"context": check, "integration_id": app} for check in checks]
    return rules


def read_ruleset(gh, name):
    """Return the ruleset called `name`, with its rules, or None."""
    for item in json.loads(gh(["api", RULESETS_API])):
        if item["name"] == name:
            return json.loads(gh(["api", f"{RULESETS_API}/{item['id']}"]))
    return None


def read_github(gh):
    """Return (labels, milestones) as GitHub has them now."""
    labels = json.loads(gh(["label", "list", "--limit", "200",
                            "--json", "name,color,description"]))
    milestones = json.loads(gh(["api", MILESTONES_API + "?state=all&per_page=100"]))
    return labels, milestones


def apply(actions, config, gh, ruleset=None, env_branches=None, owner=None):
    """Make every create and update of the plan."""
    labels = {label["name"]: label for label in config["labels"]}
    milestones = {milestone["title"]: milestone for milestone in config["milestones"]}
    managed = {spec["name"]: spec for spec in config.get("managed_rulesets", [])}
    environments = {spec["name"]: spec for spec in config.get("environments", [])}
    for action in actions:
        if action.op == "keep":
            continue
        if action.kind == "environment":
            spec = environments[action.name]
            ids = [json.loads(gh(["api", f"users/{login}"]))["id"]
                   for login in _reviewers(spec, owner)]
            body = {"reviewers": [{"type": "User", "id": i} for i in ids],
                    "prevent_self_review": False,
                    "deployment_branch_policy": {"protected_branches": False,
                                                 "custom_branch_policies": True}}
            gh(["api", "--method", "PUT", "--input", "-", f"{ENVIRONMENTS_API}/{action.name}"],
               stdin=json.dumps(body))
            have = set((env_branches or {}).get(action.name, []))
            for branch in spec["branches"]:
                if branch not in have:
                    gh(["api", "--method", "POST", "--input", "-",
                        f"{ENVIRONMENTS_API}/{action.name}/deployment-branch-policies"],
                       stdin=json.dumps({"name": branch, "type": "branch"}))
        elif action.kind == "ruleset" and action.name in managed:
            body = json.dumps(managed_body(managed[action.name]))
            if action.op == "create":
                gh(["api", "--method", "POST", "--input", "-", RULESETS_API], stdin=body)
            else:
                gh(["api", "--method", "PUT", "--input", "-", f"{RULESETS_API}/{action.number}"],
                   stdin=body)
        elif action.kind == "ruleset":
            rules = ruleset_rules(ruleset, config["ruleset"]["required_checks"])
            # GitHub refuses "parameters": null, which its own GET returns for simple rules.
            rules = [{k: v for k, v in rule.items() if v is not None} for rule in rules]
            gh(["api", "--method", "PUT", "--input", "-", f"{RULESETS_API}/{action.number}"],
               stdin=json.dumps({"rules": rules}))
        elif action.kind == "label":
            label = labels[action.name]
            # --force turns "create" into "create or update".
            gh(["label", "create", label["name"], "--color", label["color"],
                "--description", label["description"], "--force"])
        else:
            milestone = milestones[action.name]
            fields = ["-f", f"title={milestone['title']}",
                      "-f", f"description={milestone['description']}"]
            if milestone.get("due_on") is not None:
                # Noon UTC: GitHub keeps only the day, counted in Pacific time, so
                # midnight UTC would be stored as the day before.
                fields += ["-f", f"due_on={milestone['due_on']}T12:00:00Z"]
            if action.op == "create":
                gh(["api", "--method", "POST", MILESTONES_API, *fields])
            else:
                gh(["api", "--method", "PATCH", f"{MILESTONES_API}/{action.number}", *fields])


def run_gh(args, stdin=None):
    """Run the real `gh` CLI and return its stdout."""
    done = subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True, encoding="utf-8", input=stdin)
    return done.stdout


def main(argv=None, gh=run_gh):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default=str(DEFAULT_CONFIG),
                        help="labels and milestones file (default: .github/pipeline/setup.json)")
    parser.add_argument("--apply", action="store_true",
                        help="change GitHub; without it, only the plan is printed")
    args = parser.parse_args(argv)

    config = load_config(args.config)
    errors = validate(config)
    if errors:
        for error in errors:
            print(f"error: {error}")
        return 1

    try:
        labels, milestones = read_github(gh)
        actions = (plan_labels(config["labels"], labels)
                   + plan_milestones(config["milestones"], milestones))
        ruleset = None
        if "ruleset" in config:
            ruleset = read_ruleset(gh, config["ruleset"]["name"])
            actions.append(plan_ruleset(config["ruleset"], ruleset))
        for spec in config.get("managed_rulesets", []):
            actions.append(plan_managed(spec, read_ruleset(gh, spec["name"])))
        owner, env_branches = None, {}
        if config.get("environments"):
            owner = repo_owner(gh)
            for spec in config["environments"]:
                env, env_branches[spec["name"]] = read_environment(gh, spec["name"])
                actions.append(plan_environment(spec, env, env_branches[spec["name"]], owner))
        # The tag rule's DeployKey bypass covers every deploy key with write access.
        keys = write_deploy_keys(gh)
        if len(keys) > 1:
            print(f"error: {len(keys)} deploy keys can write ({', '.join(k['title'] for k in keys)}). "
                  "The tag rule lets every one of them tag; keep only the release key.")
            return 1
        for action in actions:
            print(f"{action.kind:<10} {action.op:<7} {action.name}")
        if any(action.op == "missing" for action in actions):
            print(f"error: no ruleset named '{config['ruleset']['name']}' on GitHub. "
                  "This tool only sets its required checks; create it in the repo settings.")
            return 1
        if any(action.op == "has-bypass" for action in actions):
            print(f"error: '{config['ruleset']['name']}' has bypass actors. Required checks "
                  "apply to the owner too; remove the bypass in the repo settings.")
            return 1
        pending = [action for action in actions if action.op != "keep"]
        if not pending:
            print("GitHub matches the config: nothing to do.")
        elif args.apply:
            apply(pending, config, gh, ruleset, env_branches, owner)
            print(f"{len(pending)} change(s) applied.")
        else:
            print(f"{len(pending)} change(s) pending. Dry run: nothing was changed. "
                  "Run again with --apply to make them.")
    except subprocess.CalledProcessError as failure:
        print(f"error: gh failed: {failure.stderr.strip()}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
