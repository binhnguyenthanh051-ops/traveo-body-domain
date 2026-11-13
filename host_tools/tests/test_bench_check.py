"""Tests for bench_check -- the bench verdict, from captures, by exit code (build step 8).

Always-checks per capture: records were decoded, none lost, the node booted, one banner per boot;
Node A's FBL jumped. Per CR: bench/expect.json names contract events that must (or must not)
appear. Diagnostic events are never evidence (REQ-LOG-008), so naming one is refused."""
import json
import pathlib
import struct
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from lib import log_decode as d   # noqa: E402
import bench_check                 # noqa: E402

BOOT, JUMP, ACCEPT, REJECT_MAC = 0x0001, 0x0002, 0x0101, 0x0102


def rec(seq, evt, arg0=0, arg1=0) -> bytes:
    body = bytes([d.SYNC, seq & d.SEQ_MASK]) + struct.pack("<HIIH", evt, 0, arg0, arg1)
    return body + struct.pack("<H", d.crc16(body))


def node_a(banners=1, jump=True) -> bytes:
    out = b"\n--- fbl nobuildid ---\n" * banners + rec(0, BOOT)
    if jump:
        out += rec(1, JUMP, 0x10040000)
    return out + b"\n--- app nobuildid ---\n" + rec(0, BOOT) + rec(1, ACCEPT, 0x200, 1)


def node_b() -> bytes:
    return b"\n--- actuator nobuildid ---\n" + rec(0, BOOT) + rec(1, ACCEPT, 0x200, 1)


def files(tmp_path, a=None, b=None, expect=None):
    pa, pb = tmp_path / "nodeA.bin", tmp_path / "nodeB.bin"
    pa.write_bytes(node_a() if a is None else a)
    pb.write_bytes(node_b() if b is None else b)
    args = ["--node", f"A={pa}", "--node", f"B={pb}"]
    if expect is not None:
        pe = tmp_path / "expect.json"
        pe.write_text(json.dumps(expect), encoding="utf-8")
        args += ["--expect", str(pe)]
    return args


def test_a_healthy_bench_passes(tmp_path, capsys):
    assert bench_check.main(files(tmp_path)) == 0
    assert "PASS" in capsys.readouterr().out


@pytest.mark.parametrize("a, b, problem", [
    (b"", None, "no records"),
    (node_a(banners=2), None, "banners"),
    (node_a(jump=False), None, "jump"),
    (None, rec(0, 0x0101), "no boot"),
])
def test_an_unhealthy_bench_fails(tmp_path, capsys, a, b, problem):
    assert bench_check.main(files(tmp_path, a, b)) == 1
    assert problem in capsys.readouterr().out


def test_a_lost_record_fails(tmp_path, capsys):
    a = node_a() + rec(5, ACCEPT, 0x200, 1)          # seq jumps from 1 to 5
    assert bench_check.main(files(tmp_path, a)) == 1
    assert "lost" in capsys.readouterr().out


def test_expected_events_pass_when_seen(tmp_path):
    expect = {"A": {"at_least": {"LOG_EVT_APP_JUMP": 1, "LOG_EVT_SECOC_ACCEPT": 1}},
              "B": {"none": ["LOG_EVT_SECOC_REJECT_MAC"]}}
    assert bench_check.main(files(tmp_path, expect=expect)) == 0


def test_a_missing_expected_event_fails(tmp_path, capsys):
    expect = {"B": {"at_least": {"LOG_EVT_APP_JUMP": 1}}}
    assert bench_check.main(files(tmp_path, expect=expect)) == 1
    assert "LOG_EVT_APP_JUMP" in capsys.readouterr().out


def test_a_forbidden_event_fails(tmp_path, capsys):
    b = node_b() + rec(2, REJECT_MAC, 0x200, 0)
    expect = {"B": {"none": ["LOG_EVT_SECOC_REJECT_MAC"]}}
    assert bench_check.main(files(tmp_path, b=b, expect=expect)) == 1


@pytest.mark.parametrize("expect", [
    {"A": {"at_least": {"LOG_EVT_NO_SUCH_EVENT": 1}}},
    {"C": {"none": []}},
    {"A": {"sometimes": {}}},
])
def test_an_invalid_expect_file_is_exit_2(tmp_path, expect):
    assert bench_check.main(files(tmp_path, expect=expect)) == 2


def test_a_diagnostic_event_cannot_be_evidence(tmp_path, capsys):
    diagnostic = next(n for i, (n, cls, *_) in bench_check.log_events.EVENTS.items()
                      if cls != "contract")
    assert bench_check.main(files(tmp_path, expect={"A": {"at_least": {diagnostic: 1}}})) == 2
    assert "diagnostic" in capsys.readouterr().out
