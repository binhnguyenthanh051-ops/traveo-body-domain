"""Tests for bench_run -- the scripted bench run for one CR (build step 8).

A script, not an agent: preflight, the PR's exact commit in the bench clone, captures, flash,
reset, verdict by bench_check's exit code, then a `bench` commit status and a PR comment. The
hardware is faked: every step goes through injected callables, so the order and the stop rules
are tested without a board."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import bench_run  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"


class Gh:
    def __init__(self, labels=("level:on-target", "needs-bench"), prs=None):
        self.labels = labels
        self.prs = [{"number": 45, "headRefOid": SHA}] if prs is None else prs
        self.calls, self.stdin = [], []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        self.stdin.append(stdin)
        if args[0] == "api" and args[1].endswith("/labels"):
            return json.dumps([{"name": n} for n in self.labels])
        if args[:2] == ["pr", "list"]:
            return json.dumps(self.prs)
        return ""

    def status(self):
        posts = [c for c in self.calls if "context=bench" in c]
        return [a.split("=", 1)[1] for c in posts for a in c if a.startswith("state=")]


class Bench:
    """Fake hardware and tools: records every step; a step can be told to fail."""

    def __init__(self, fail=None, check_rc=0):
        self.steps, self.fail, self.check_rc = [], fail, check_rc

    def run(self, cmd, cwd):
        name = " ".join(cmd)
        self.steps.append(name)
        if self.fail and self.fail in name:
            return 1
        if "bench_check.py" in name:
            return self.check_rc
        return 0

    def capture(self, port, path):
        self.steps.append(f"capture {port}")
        bench = self

        class Handle:
            def stop(self):
                bench.steps.append(f"stop {port}")
        return Handle()


def go(tmp_path, gh, bench):
    return bench_run.run_bench(
        43, gh=gh, run=bench.run, capture=bench.capture, wait=lambda s: None,
        clone=tmp_path / "bench-clone", source=tmp_path / "repo", today="2026-11-14")


@pytest.fixture
def repo(tmp_path):
    (tmp_path / "repo").mkdir()
    (tmp_path / "repo" / "bench.env").write_text("NODE_A_PROBE=x\n", encoding="utf-8")
    (tmp_path / "repo" / "ec_p256_dev_private.pem").write_text("k", encoding="utf-8")
    return tmp_path


def test_a_cr_that_is_not_needs_bench_is_refused_before_anything_runs(repo):
    gh, bench = Gh(labels=("level:on-target", "review")), Bench()
    assert go(repo, gh, bench) == 2
    assert bench.steps == [] and gh.status() == []


def test_a_cr_without_an_open_pr_is_refused(repo):
    gh, bench = Gh(prs=[]), Bench()
    assert go(repo, gh, bench) == 2
    assert bench.steps == []


def test_the_happy_path_runs_in_the_bench_rule_order_and_posts_success(repo):
    gh, bench = Gh(), Bench()
    assert go(repo, gh, bench) == 0
    steps = bench.steps
    order = [next(i for i, s in enumerate(steps) if key in s) for key in (
        f"--detach {SHA}", "capture COM4", "capture COM6",
        "zz_build_gateway_fbl.sh program", "zz_build_gateway_app.sh program",
        "zz_build_actuator_app.sh program", "reset_node.sh A", "reset_node.sh B",
        "stop COM4", "bench_check.py")]
    assert order == sorted(order)
    assert gh.status() == ["pending", "success"]
    assert any(c[:2] == ["pr", "comment"] for c in gh.calls)


def test_the_bench_clone_gets_the_local_bench_files(repo):
    go(repo, Gh(), Bench())
    assert (repo / "bench-clone" / "bench.env").read_text(encoding="utf-8") == "NODE_A_PROBE=x\n"


def test_the_record_goes_in_the_day_folder_of_the_source_repo(repo):
    go(repo, Gh(), Bench())
    assert (repo / "repo" / "docs" / "bench" / "2026-11-14" / "cr-43").is_dir()


def test_a_failed_flash_stops_the_session_and_posts_failure(repo):
    gh, bench = Gh(), Bench(fail="zz_build_gateway_app.sh")
    assert go(repo, gh, bench) == 1
    assert not any("zz_build_actuator_app.sh" in s for s in bench.steps)
    assert not any("bench_check.py" in s for s in bench.steps)
    assert any(s.startswith("stop ") for s in bench.steps)        # captures always stop
    assert gh.status() == ["pending", "failure"]


def test_a_failed_check_posts_failure(repo):
    gh, bench = Gh(), Bench(check_rc=1)
    assert go(repo, gh, bench) == 1
    assert gh.status() == ["pending", "failure"]


def test_the_bench_token_comes_from_its_own_file_as_gh_token(tmp_path, monkeypatch):
    # A second `gh` login shares the Windows keyring with the owner's (found 2026-11-14), so the
    # bench never logs in: it hands its token to each gh call as GH_TOKEN.
    token = tmp_path / "token"
    token.write_text("github_pat_x \n", encoding="utf-8")
    monkeypatch.setenv("BENCH_TOKEN_FILE", str(token))
    seen = {}

    def fake_run(cmd, **kw):
        seen.update(kw["env"])

        class R:
            stdout = ""
        return R()
    monkeypatch.setattr(bench_run.subprocess, "run", fake_run)
    bench_run.run_gh(["api", "user"])
    assert seen["GH_TOKEN"] == "github_pat_x"
    assert "GH_CONFIG_DIR" not in seen or seen["GH_CONFIG_DIR"] != str(tmp_path)


def test_a_missing_token_file_is_a_clear_error(tmp_path, monkeypatch):
    monkeypatch.setenv("BENCH_TOKEN_FILE", str(tmp_path / "none"))
    with pytest.raises(SystemExit, match="bench token"):
        bench_run.run_gh(["api", "user"])


# --- release mode: the full on-target regression on the release commit (build step 9) -----

def release(tmp_path, gh, bench):
    return bench_run.run_release(
        SHA, gh=gh, run=bench.run, capture=bench.capture, wait=lambda s: None,
        clone=tmp_path / "bench-clone", source=tmp_path / "repo", today="2026-11-14")


def release_states(gh):
    posts = [c for c in gh.calls if "context=bench-release" in c]
    return [a.split("=", 1)[1] for c in posts for a in c if a.startswith("state=")]


def test_release_mode_runs_the_session_and_posts_bench_release(repo):
    gh, bench = Gh(), Bench()
    assert release(repo, gh, bench) == 0
    assert release_states(gh) == ["pending", "success"]
    assert not any(c[:2] == ["pr", "comment"] for c in gh.calls)
    assert any("zz_build_actuator_app.sh program" in s for s in bench.steps)
    assert (repo / "repo" / "docs" / "bench" / "2026-11-14" / f"release-{SHA[:7]}").is_dir()


def test_release_mode_refuses_a_commit_that_is_not_on_main(repo):
    gh, bench = Gh(), Bench(fail="merge-base")
    assert release(repo, gh, bench) == 2
    assert not any("zz_build" in s for s in bench.steps)
    assert release_states(gh) == []


def test_release_mode_fails_on_a_failed_check(repo):
    gh, bench = Gh(), Bench(check_rc=1)
    assert release(repo, gh, bench) == 1
    assert release_states(gh) == ["pending", "failure"]


def test_never_erase_or_touch_supervisory_flash(repo):
    bench = Bench()
    go(repo, Gh(), bench)
    # Command words only: pytest's temp folder carries this test's name, "erase" included.
    words = [w for s in bench.steps for w in s.split() if "/" not in w and "\\" not in w]
    assert not any(bad in w for w in words for bad in ("erase", "efuse", "sflash", "--update-kp3"))
