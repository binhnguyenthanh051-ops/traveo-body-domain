"""test_logview.py -- the logview CLI's output encoding (F-005).

`logview --file ... > out.txt` crashed on Windows: the decoder turns a
non-ASCII text byte into U+FFFD, and a redirected stdout there is cp1252, which
cannot encode it. The test forces that codepage with PYTHONIOENCODING, so it
reproduces the Windows failure on any host.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

LOGVIEW = Path(__file__).resolve().parent.parent / "logview" / "logview.py"


def run_logview(capture: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONIOENCODING="cp1252")
    return subprocess.run([sys.executable, str(LOGVIEW), "--file", str(capture)],
                          capture_output=True, env=env, check=False)


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
