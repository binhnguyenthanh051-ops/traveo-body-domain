"""Tests for bench_gate -- makes the `bench` check required only where the board is needed.

`bench` is a required check on main. For every pull request CI posts it: success for non-CRs
and host-only CRs, pending for on-target CRs until the bench run posts the real result. The
decision reads the CR's labels only (design note section 3: labels, never text)."""
import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import bench_gate  # noqa: E402


@pytest.mark.parametrize("branch, labels, state", [
    ("ci/pipeline-08-bench", [], "success"),          # not a CR
    ("cr/43", ["bug", "level:host"], "success"),
    ("cr/43", ["level:on-target"], "pending"),
    ("cr/43", ["level:both"], "pending"),
    ("cr/43", ["bug"], "pending"),                     # no level: block until the owner sets one
])
def test_decide(branch, labels, state):
    got, why = bench_gate.decide(branch, labels)
    assert got == state and why


class FakeGh:
    def __init__(self, labels):
        self.labels, self.calls = labels, []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        if args[0] == "api" and "/labels" in args[1]:
            return json.dumps([{"name": n} for n in self.labels])
        return ""


def test_main_posts_the_bench_status_on_the_head_commit():
    gh = FakeGh(["level:on-target"])
    assert bench_gate.main(["--branch", "cr/43", "--sha", "abc123"], gh=gh) == 0
    (post,) = [c for c in gh.calls if "--method" in c]
    assert post[post.index("--method") + 1] == "POST"
    assert any(a.endswith("/statuses/abc123") for a in post)
    assert "state=pending" in post and "context=bench" in post


def test_main_does_not_read_labels_for_a_non_cr_branch():
    gh = FakeGh([])
    bench_gate.main(["--branch", "main", "--sha", "abc123"], gh=gh)
    assert not any("/labels" in " ".join(c) for c in gh.calls)
