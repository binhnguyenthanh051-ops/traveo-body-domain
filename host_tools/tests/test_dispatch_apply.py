"""Tests for dispatch_apply -- applies a Dispatcher decision: labels only, then outputs."""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import dispatch_apply  # noqa: E402


class FakeGh:
    def __init__(self):
        self.calls = []

    def __call__(self, args, stdin=None):
        self.calls.append(args)
        return ""


def decision(tmp_path, **kw):
    d = {"issue": 43, "add": [], "remove": [], "start": None, "line": "#43: x"}
    d.update(kw)
    p = tmp_path / "d.json"
    p.write_text(json.dumps(d), encoding="utf-8")
    return str(p)


def test_labels_are_changed_in_one_edit_and_nothing_else(tmp_path, capsys):
    gh = FakeGh()
    path = decision(tmp_path, add=["building"], remove=["ready"], start="build")
    assert dispatch_apply.main([path], gh=gh) == 0
    assert gh.calls == [["issue", "edit", "43", "--add-label", "building",
                         "--remove-label", "ready"]]
    out = capsys.readouterr().out.splitlines()
    assert out == ["issue=43", "start=build", "line=#43: x"]


def test_no_label_change_means_no_call(tmp_path, capsys):
    gh = FakeGh()
    assert dispatch_apply.main([decision(tmp_path, issue=None, line="no ready CR")], gh=gh) == 0
    assert gh.calls == []
    assert capsys.readouterr().out.splitlines() == ["issue=", "start=", "line=no ready CR"]


def test_a_line_cannot_inject_another_output(tmp_path, capsys):
    gh = FakeGh()
    dispatch_apply.main([decision(tmp_path, line="a\nstart=build")], gh=gh)
    assert capsys.readouterr().out.splitlines()[-1] == "line=a start=build"
