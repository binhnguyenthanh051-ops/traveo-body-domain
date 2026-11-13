"""Tests for test_first -- the scripted "tests before code" evidence the Inspector reads.

Design note section 2: the Builder's first commit holds only tests, and they must fail; the
pull request shows this, so "test before code" is evidence and not a promise. The verdict on
the evidence comes from exit codes, never from an agent."""
import json
import pathlib
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import test_first  # noqa: E402


@pytest.mark.parametrize("path, expected", [
    ("host_tools/tests/test_x.py", True),
    ("shared/log/tests/test_log.c", True),
    ("shared/log/tests/log_port_fake.c", True),
    ("tests/conftest.py", True),
    ("host_tools/test_first.py", False),           # a tool, not a test
    ("shared/log/src/log.c", False),
    ("docs/requirements/logging.md", False),
])
def test_what_counts_as_a_test_file(path, expected):
    assert test_first.is_test_path(path) is expected


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def repo(tmp_path):
    """main has check.py; the 'PR' adds a test that fails, then the code that passes it."""
    r = tmp_path / "r"
    r.mkdir()
    git(r, "init", "-q", "-b", "main")
    git(r, "config", "user.email", "t@t")
    git(r, "config", "user.name", "t")
    (r / "app.py").write_text("VALUE = 1\n", encoding="utf-8")
    (r / "check.py").write_text(
        "import sys, pathlib\n"
        "t = pathlib.Path('tests/test_value.py')\n"
        "sys.exit(0 if not t.exists() or 'VALUE = 2' in pathlib.Path('app.py').read_text() else 1)\n",
        encoding="utf-8")
    git(r, "add", ".")
    git(r, "commit", "-q", "-m", "base")
    base = git(r, "rev-parse", "HEAD")
    (r / "tests").mkdir()
    (r / "tests" / "test_value.py").write_text("# expects VALUE = 2\n", encoding="utf-8")
    git(r, "add", ".")
    git(r, "commit", "-q", "-m", "test first")
    (r / "app.py").write_text("VALUE = 2\n", encoding="utf-8")
    git(r, "commit", "-q", "-am", "code")
    return r, base


CMD = [sys.executable, "check.py"]


def test_a_tests_first_pr_is_shown_as_such(repo):
    r, base = repo
    ev = test_first.evidence(r, base, "HEAD", CMD)
    assert ev["tests_only_first_commit"] is True
    assert ev["first_commit_fails"] is True
    assert ev["head_passes"] is True
    assert ev["holds"] is True


def test_code_in_the_first_commit_breaks_the_evidence(repo):
    r, base = repo
    git(r, "reset", "-q", "--soft", base)
    git(r, "commit", "-q", "-m", "all at once")
    ev = test_first.evidence(r, base, "HEAD", CMD)
    assert ev["tests_only_first_commit"] is False
    assert ev["holds"] is False


def test_a_first_commit_that_passes_proves_nothing(repo):
    r, base = repo
    ev = test_first.evidence(r, base, "HEAD", [sys.executable, "-c", "pass"])
    assert ev["first_commit_fails"] is False
    assert ev["holds"] is False


def test_the_worktree_is_removed_afterwards(repo):
    r, base = repo
    test_first.evidence(r, base, "HEAD", CMD)
    assert git(r, "worktree", "list").count("\n") == 0


def test_main_prints_json_and_exits_0_even_when_it_does_not_hold(repo, capsys):
    # The Inspector judges; this script only reports.
    r, base = repo
    rc = test_first.main(["--repo", str(r), "--base", base, "--head", "HEAD", "--",
                          sys.executable, "-c", "pass"])
    assert rc == 0
    assert json.loads(capsys.readouterr().out)["holds"] is False
