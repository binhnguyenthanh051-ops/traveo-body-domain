"""Tests for bundle_check -- what the Builder's publish job checks before it pushes.

The agent's commits reach the publish job as a git bundle. The Challenger found (2026-11-13)
that publish only checked protected paths. Each rule here gets a real temporary git repo: the
bundle must verify, carry exactly the CR's branch, fast-forward from the base the build started
on, hold no merge commits, stay within the commit and size caps, and add no symlink or
submodule."""
import pathlib
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import bundle_check  # noqa: E402


def git(repo, *args, stdin=None):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                          text=True, input=stdin).stdout.strip()


@pytest.fixture
def repo(tmp_path):
    r = tmp_path / "r"
    r.mkdir()
    git(r, "init", "-q", "-b", "main")
    git(r, "config", "user.email", "t@t")
    git(r, "config", "user.name", "t")
    git(r, "config", "core.autocrlf", "false")
    (r / "a.txt").write_text("a\n", encoding="utf-8")
    git(r, "add", ".")
    git(r, "commit", "-q", "-m", "base")
    git(r, "switch", "-q", "-c", "cr/7")
    return r


def commit(repo, name="b.txt", text="b\n", message="change"):
    (repo / name).write_text(text, encoding="utf-8")
    git(repo, "add", name)
    git(repo, "commit", "-q", "-m", message)


def bundle(repo, *refs, base="main"):
    path = repo.parent / "cr.bundle"
    git(repo, "bundle", "create", str(path), *(refs or ("cr/7",)), f"^{base}")
    return path


def check(repo, path, base=None, **caps):
    base = base or git(repo, "rev-parse", "main")
    return bundle_check.check(repo, path, "cr/7", base, **caps)


def test_a_clean_bundle_passes(repo):
    commit(repo)
    commit(repo, "c.txt", "c\n")
    assert check(repo, bundle(repo)) == []


def test_the_temporary_ref_is_removed(repo):
    commit(repo)
    check(repo, bundle(repo))
    assert "bundle-check" not in git(repo, "for-each-ref")


def test_a_corrupt_bundle_fails(repo, tmp_path):
    bad = tmp_path / "bad.bundle"
    bad.write_bytes(b"not a bundle\n")
    assert any("does not verify" in p for p in check(repo, bad))


def test_an_extra_ref_fails(repo):
    commit(repo)
    git(repo, "branch", "other")
    assert any("exactly one ref" in p for p in check(repo, bundle(repo, "cr/7", "other")))


def test_the_wrong_branch_name_fails(repo):
    commit(repo)
    git(repo, "branch", "-m", "cr/7", "cr/8")
    path = repo.parent / "cr.bundle"
    git(repo, "bundle", "create", str(path), "cr/8", "^main")
    assert any("refs/heads/cr/7" in p for p in check(repo, path))


def test_a_branch_that_does_not_start_from_the_base_fails(repo):
    commit(repo)
    git(repo, "switch", "-q", "main")
    commit(repo, "m.txt", "m\n", "main moved")
    moved = git(repo, "rev-parse", "main")
    git(repo, "switch", "-q", "cr/7")
    path = repo.parent / "cr.bundle"
    git(repo, "bundle", "create", str(path), "cr/7", "^main~1")
    assert any("does not fast-forward" in p for p in check(repo, path, base=moved))


def test_a_merge_commit_fails(repo):
    commit(repo)
    git(repo, "switch", "-q", "-c", "side", "main")
    commit(repo, "s.txt", "s\n")
    git(repo, "switch", "-q", "cr/7")
    git(repo, "merge", "-q", "--no-ff", "-m", "merge", "side")
    assert any("merge commit" in p for p in check(repo, bundle(repo)))


def test_too_many_commits_fails(repo):
    for n in range(4):
        commit(repo, f"f{n}.txt", f"{n}\n")
    assert any("4 commits" in p for p in check(repo, bundle(repo), max_commits=3))


def test_an_empty_bundle_range_fails(repo):
    path = repo.parent / "cr.bundle"
    commit(repo)
    git(repo, "bundle", "create", str(path), "cr/7", "^main")
    head = git(repo, "rev-parse", "cr/7")
    assert any("no new commits" in p for p in check(repo, path, base=head))


def test_a_large_file_fails(repo):
    commit(repo, "big.bin", "x" * 3000)
    assert any("big.bin" in p for p in check(repo, bundle(repo), max_file_bytes=2000))


def test_a_large_diff_fails(repo):
    commit(repo, "a1.txt", "y" * 1500)
    commit(repo, "a2.txt", "z" * 1500)
    assert any("diff" in p for p in check(repo, bundle(repo), max_diff_bytes=2000))


@pytest.mark.parametrize("mode, what", [("120000", "symlink"), ("160000", "submodule")])
def test_a_symlink_or_submodule_fails(repo, mode, what):
    blob = git(repo, "hash-object", "-w", "--stdin", stdin="target\n")
    obj = blob if mode == "120000" else git(repo, "rev-parse", "main")
    git(repo, "update-index", "--add", "--cacheinfo", f"{mode},{obj},link")
    git(repo, "commit", "-q", "-m", what)
    assert any(what in p for p in check(repo, bundle(repo)))


def test_main_exit_codes(repo, capsys):
    commit(repo)
    path = bundle(repo)
    base = git(repo, "rev-parse", "main")
    args = ["--repo", str(repo), "--bundle", str(path), "--branch", "cr/7", "--base", base]
    assert bundle_check.main(args) == 0
    assert bundle_check.main(args + ["--max-commits", "0"]) == 1
    assert "commits" in capsys.readouterr().out
