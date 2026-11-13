#!/usr/bin/env python3
"""Scripted evidence for "tests before code" (docs/design/agent-pipeline.md section 2).

    python host_tools/test_first.py --repo . --base <sha> --head <ref> -- <test command...>

Checks three facts about the commits base..head, and prints them as JSON:
  tests_only_first_commit  the first commit changes test files only (a path under a tests/ folder)
  first_commit_fails       the test command fails at the first commit
  head_passes              the test command passes at the head
  holds                    all three
Each commit is checked out in a temporary git worktree, removed afterwards. The script only
reports and always exits 0: the Inspector weighs the evidence, and the facts come from exit
codes, not from an agent.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def is_test_path(path):
    return "tests" in pathlib.PurePosixPath(path.replace("\\", "/")).parts[:-1]


def _git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def _passes(repo, commit, command):
    with tempfile.TemporaryDirectory() as tmp:
        tree = pathlib.Path(tmp) / "tree"
        _git(repo, "worktree", "add", "--quiet", "--detach", str(tree), commit)
        try:
            return subprocess.run(command, cwd=tree, capture_output=True).returncode == 0
        finally:
            _git(repo, "worktree", "remove", "--force", str(tree))


def evidence(repo, base, head, command):
    commits = _git(repo, "rev-list", "--reverse", f"{base}..{head}").split()
    if not commits:
        return {"commits": 0, "tests_only_first_commit": False, "first_commit_fails": False,
                "head_passes": False, "holds": False}
    first = commits[0]
    files = _git(repo, "diff-tree", "--no-commit-id", "--name-only", "-r", first).split()
    tests_only = bool(files) and all(is_test_path(f) for f in files)
    fails = not _passes(repo, first, command)
    passes = _passes(repo, commits[-1], command)
    return {"commits": len(commits), "first_commit": first[:7], "first_commit_files": files,
            "tests_only_first_commit": tests_only, "first_commit_fails": fails,
            "head_passes": passes, "holds": tests_only and fails and passes}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".")
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("command", nargs="+", help="the test command, after --")
    args = parser.parse_args(argv)
    print(json.dumps(evidence(args.repo, args.base, args.head, args.command)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
