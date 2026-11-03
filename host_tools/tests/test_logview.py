"""test_logview.py -- the logview CLI: output encoding (F-005) and ACCEPT folding.

`logview --file capture.bin > out.txt` crashed on Windows: the decoder turns a
non-ASCII text byte into U+FFFD, and a redirected stdout there is cp1252, which
cannot encode it. The test forces that codepage with PYTHONIOENCODING, so it
reproduces the Windows failure on any host.

ACCEPT folding is display only: one SECOC_ACCEPT per secured frame (REQ-LOG-009)
is ~50 lines/s on Node A, so logview folds them into one line per CAN ID per
window. The rule these tests pin: ONLY accepts fold. A reject, a resync, a gap or
a reboot must never hide inside a summary.
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "logview"))

from lib import log_decode as d      # noqa: E402
import logview                       # noqa: E402

LOGVIEW = Path(__file__).resolve().parent.parent / "logview" / "logview.py"

ACCEPT, REJECT_FRESH = 0x0101, 0x0103
BOOT = d.BOOT_EVT_ID


def run_logview(capture: Path, *extra: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONIOENCODING="cp1252")
    return subprocess.run([sys.executable, str(LOGVIEW), "--file", str(capture), *extra],
                          capture_output=True, env=env, check=False)


def rec(evt=ACCEPT, ts=0, can_id=0x200, ctr=0, seq=0) -> d.Record:
    return d.Record(core=0, seq=seq, evt=evt, ts_ms=ts, arg0=can_id, arg1=ctr)


def fold(items, window_ms=5000) -> list[str]:
    folder = logview.AcceptFolder(window_ms)
    out = folder.feed(items) + folder.flush()
    return [str(i) for i in out]


def encode(core=0, seq=0, evt=ACCEPT, ts=0, arg0=0, arg1=0) -> bytes:
    """Mirror of log_rec_encode() in shared/log/src/log.c."""
    body = bytes([d.SYNC, (core << d.CORE_SHIFT) | (seq & d.SEQ_MASK)])
    body += struct.pack("<HIIH", evt, ts, arg0, arg1)
    return body + struct.pack("<H", d.crc16(body))


def test_undecodable_text_byte_does_not_crash_a_redirected_cp1252_stdout(tmp_path):
    capture = tmp_path / "capture.bin"
    capture.write_bytes(b"--- fbl \x8f banner ---\r\n")

    res = run_logview(capture)

    assert res.returncode == 0, res.stderr.decode(errors="replace")
    out = res.stdout.decode("utf-8")              # UTF-8, whatever the codepage
    assert "--- fbl � banner ---" in out     # the bad byte is visible, not dropped
    assert "0 records" in out


def test_plain_ascii_capture_is_unchanged(tmp_path):
    capture = tmp_path / "capture.bin"
    capture.write_bytes(b"--- app nobuildid ---\r\n")

    res = run_logview(capture)

    assert res.returncode == 0
    assert res.stdout.decode("utf-8").startswith("--- app nobuildid ---")


# --- ACCEPT folding --------------------------------------------------------

def test_accepts_in_one_window_fold_into_one_line():
    lines = fold([rec(ts=0, ctr=1), rec(ts=20, ctr=2), rec(ts=40, ctr=3)])
    assert len(lines) == 1
    assert "x3" in lines[0] and "can_id=0x200" in lines[0] and "1..3" in lines[0]


def test_a_reject_is_never_folded():
    lines = fold([rec(ts=0, ctr=1), rec(evt=REJECT_FRESH, ts=10, ctr=1), rec(ts=20, ctr=2)])
    rejects = [line for line in lines if "REJECT_FRESHNESS" in line]
    assert len(rejects) == 1
    assert "x2" in lines[-1]                     # both accepts still counted


def test_each_can_id_gets_its_own_line():
    lines = fold([rec(ts=0, can_id=0x200, ctr=5), rec(ts=10, can_id=0x2F0, ctr=1)])
    assert len(lines) == 2
    assert any("0x200" in line for line in lines) and any("0x2F0" in line for line in lines)


def test_window_boundary_emits_the_summary_before_the_next_accept():
    folder = logview.AcceptFolder(5000)
    assert folder.feed([rec(ts=0, ctr=1), rec(ts=4999, ctr=2)]) == []
    out = [str(i) for i in folder.feed([rec(ts=5000, ctr=3)])]
    assert len(out) == 1 and "x2" in out[0]      # ctr 3 opens the next window
    assert "x1" in str(folder.flush()[0])


def test_text_flushes_pending_accepts_first():
    """A banner means a reboot: the accepts before it must print before it."""
    lines = fold([rec(ts=0, ctr=1), d.Text("\n--- app nobuildid ---\n")])
    assert "x1" in lines[0] and "--- app" in lines[1]


def test_a_gap_flushes_pending_accepts_first():
    lines = fold([rec(ts=0, ctr=1), d.Gap(0, 3)])
    assert "x1" in lines[0] and "LOST" in lines[1]


def test_a_boot_record_flushes_pending_accepts_first():
    lines = fold([rec(ts=9000, ctr=1), rec(evt=BOOT, ts=0)])
    assert "x1" in lines[0] and "LOG_EVT_BOOT" in lines[1]


def test_timestamp_going_backwards_flushes():
    """A reset whose banner/BOOT was lost still shows as ts restarting."""
    lines = fold([rec(ts=9000, ctr=7), rec(ts=10, ctr=1)])
    assert len(lines) == 2 and "7..7" in lines[0] and "1..1" in lines[1]


def test_cli_folds_by_default_and_all_expands(tmp_path):
    capture = tmp_path / "capture.bin"
    capture.write_bytes(b"".join(encode(seq=i, ts=i * 20, arg0=0x200, arg1=i + 1)
                                 for i in range(10)))

    folded = run_logview(capture).stdout.decode("utf-8")
    expanded = run_logview(capture, "--all").stdout.decode("utf-8")

    assert folded.count("SECOC_ACCEPT") == 1 and "x10" in folded
    assert expanded.count("SECOC_ACCEPT") == 10
    assert "10 records" in folded                # the summary still counts every one
