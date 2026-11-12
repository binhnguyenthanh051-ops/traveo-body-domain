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
    have = {check["context"] for check in _checks_rule(existing["rules"])
            .get("parameters", {}).get("required_status_checks", [])}
    op = "keep" if have == set(wanted["required_checks"]) else "update"
    return Action("ruleset", op, wanted["name"], existing["id"])


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


def apply(actions, config, gh, ruleset=None):
    """Make every create and update of the plan."""
    labels = {label["name"]: label for label in config["labels"]}
    milestones = {milestone["title"]: milestone for milestone in config["milestones"]}
    for action in actions:
        if action.op == "keep":
            continue
        if action.kind == "ruleset":
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
        for action in actions:
            print(f"{action.kind:<10} {action.op:<7} {action.name}")
        if any(action.op == "missing" for action in actions):
            print(f"error: no ruleset named '{config['ruleset']['name']}' on GitHub. "
                  "This tool only sets its required checks; create it in the repo settings.")
            return 1
        pending = [action for action in actions if action.op != "keep"]
        if not pending:
            print("GitHub matches the config: nothing to do.")
        elif args.apply:
            apply(pending, config, gh, ruleset)
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
