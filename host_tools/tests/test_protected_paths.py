"""Tests for protected_paths -- the CI check behind docs/design/agent-pipeline.md section 7.

A pull request that touches a protected path fails, unless the owner added the label
`protected-change-approved`. The list of protected paths lives in
.github/pipeline/protected-paths.txt; these tests pin it against section 7, and pin the
glob rules the checker uses to read it."""
import pathlib
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import protected_paths  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]

# Section 7, one example per bullet, plus the checker's own files.
PROTECTED = [
    "docs/requirements/secoc.md",                                   # requirements
    "docs/design/agent-pipeline.md",                                # design notes
    "docs/architecture/overview.md",                                # architecture
    "docs/architecture/decisions/ADR-0001-host-testable-core.md",   # ADRs
    ".github/workflows/ci.yml",                                     # CI workflows
    "node_a_gateway/app/linker/app_cm4.ld",                         # linker scripts
    "security/include/secoc_shared_secret.h",                       # keys
    "node_a_gateway/bootloader/proj_cm0p/src/crypto_pubkey_dev.h",
    "shared/crypto/include/crypto_keystore.h",
    "shared/crypto/src/crypto_keystore.c",
    "host_tools/gen_dev_key.py",
    "host_tools/sign_image.py",
    "host_tools/check_key_residency.py",
    ".gitignore",
    "docs/project/plan.md",                                         # plan and week files
    "docs/project/weeks/2026-W46.md",
    "CLAUDE.md",
    ".claude/settings.json",                                        # agent definitions
    ".claude/skills/bench-test/SKILL.md",
    ".claude/agents/builder.md",
    ".github/prompts/run-checks.prompt.md",
    ".github/pipeline/protected-paths.txt",                         # this check itself
    "host_tools/protected_paths.py",
    "host_tools/pipeline_setup.py",
    "host_tools/cr_queue.py",
    "host_tools/dispatcher.py",
    "host_tools/dispatch_apply.py",
    "host_tools/test_first.py",
    "host_tools/planner_publish.py",
]

NOT_PROTECTED = [
    "shared/crypto/src/crypto_service.c",      # only the keystore pair is protected
    "shared/crypto/tests/test_crypto_keystore.c",
    "shared/secoc/src/secoc.c",
    "host_tools/trace_report.py",
    "host_tools/tests/test_protected_paths.py",
    "docs/bench/2026-11-12/nodeA.txt",
    "docs/blogs/m5-post.md",
    "docs/architecture/secoc-architecture.md",
    "README.md",
    "Makefile",
]


@pytest.fixture(scope="module")
def patterns():
    return protected_paths.load_patterns(protected_paths.DEFAULT_PATTERNS)


@pytest.mark.parametrize("path", PROTECTED)
def test_section_7_paths_are_protected(patterns, path):
    assert protected_paths.protected(path, patterns)


@pytest.mark.parametrize("path", NOT_PROTECTED)
def test_ordinary_paths_are_not_protected(patterns, path):
    assert not protected_paths.protected(path, patterns)


def test_every_linker_script_in_the_repo_is_protected(patterns):
    files = subprocess.run(["git", "ls-files", "*.ld", "*.sct", "*.icf"], cwd=REPO,
                           capture_output=True, text=True, check=True).stdout.split()
    assert len(files) >= 20                                   # 22 on 2026-11-12
    assert [f for f in files if not protected_paths.protected(f, patterns)] == []


# --- the glob rules -------------------------------------------------------------------

@pytest.mark.parametrize("pattern, path, expected", [
    ("docs/design/", "docs/design/a.md", True),             # trailing / = whole folder
    ("docs/design/", "docs/design/sub/b.md", True),
    ("docs/design/", "docs/designs/a.md", False),
    ("docs/design/", "docs/design", False),
    ("CLAUDE.md", "CLAUDE.md", True),                        # no / = exact path
    ("CLAUDE.md", "docs/CLAUDE.md", False),
    ("**/*.ld", "a.ld", True),                               # ** = zero or more folders
    ("**/*.ld", "a/b/c.ld", True),
    ("**/*.ld", "a/b/c.ldx", False),
    ("docs/*.md", "docs/a.md", True),                        # * stays inside one folder
    ("docs/*.md", "docs/x/a.md", False),
])
def test_glob_rules(pattern, path, expected):
    assert protected_paths.protected(path, [pattern]) is expected


def test_windows_separators_are_normalised():
    assert protected_paths.protected("docs\\design\\a.md", ["docs/design/"])


def test_comments_and_blank_lines_are_ignored(tmp_path):
    f = tmp_path / "p.txt"
    f.write_text("# a comment\n\n  docs/design/  \n", encoding="utf-8")
    assert protected_paths.load_patterns(f) == ["docs/design/"]


# --- main: the verdict is the exit code -------------------------------------------------

def changed(tmp_path, *paths):
    f = tmp_path / "changed.txt"
    f.write_text("\n".join(paths) + "\n", encoding="utf-8")
    return str(f)


def test_no_protected_path_passes(tmp_path, capsys):
    assert protected_paths.main(["--changed", changed(tmp_path, "README.md")]) == 0
    assert "no protected path" in capsys.readouterr().out


def test_a_protected_path_fails_and_names_the_file(tmp_path, capsys):
    rc = protected_paths.main(["--changed", changed(tmp_path, "README.md", "CLAUDE.md")])
    out = capsys.readouterr().out
    assert rc == 1
    assert "CLAUDE.md" in out and "README.md" not in out
    assert "protected-change-approved" in out


def test_the_label_lets_a_protected_path_pass(tmp_path, capsys):
    rc = protected_paths.main(["--changed", changed(tmp_path, "CLAUDE.md"), "--approved"])
    assert rc == 0
    assert "CLAUDE.md" in capsys.readouterr().out          # still listed, for the record


def test_a_rename_out_of_a_protected_folder_is_caught(tmp_path):
    # The workflow lists both names of a renamed file.
    path = changed(tmp_path, "docs/notes/agent-pipeline.md", "docs/design/agent-pipeline.md")
    assert protected_paths.main(["--changed", path]) == 1


def test_an_empty_list_fails_closed(tmp_path, capsys):
    # No changed files means the file list step broke, not that the PR is empty.
    assert protected_paths.main(["--changed", changed(tmp_path)]) == 2
    assert "no changed files" in capsys.readouterr().out
