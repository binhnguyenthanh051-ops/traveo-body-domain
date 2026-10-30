"""Tests for check_key_residency -- the REQ-SECOC-011 evidence check. Uses
synthetic images, so it runs in CI without any MTB build output."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from check_key_residency import KEY_HEADER, ROOT, check, read_key  # noqa: E402

KEY = bytes(range(0x40, 0x50))


def _img(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return p


def test_reads_the_16_byte_key_from_the_real_header():
    assert len(read_key(KEY_HEADER)) == 16


def test_parses_bytes_and_ignores_comments(tmp_path):
    h = tmp_path / "k.h"
    h.write_text("static const uint8_t secoc_shared_secret[16u] = {\n"
                 + ", ".join(f"0x{b:02X}u" for b in KEY[:8]) + ", /* 0xFFu */\n"
                 + ", ".join(f"0x{b:02X}u" for b in KEY[8:]) + "\n};\n", encoding="utf-8")
    assert read_key(h) == KEY


def test_cm0p_with_key_and_app_without_pass(tmp_path, monkeypatch):
    monkeypatch.setattr("check_key_residency.ROOT", tmp_path)
    lines = check([("cm0p", _img(tmp_path, "m0.elf", b"\0" * 7 + KEY + b"\0")),
                   ("app", _img(tmp_path, "app.elf", b"\0" * 64))], KEY)
    assert all(ln.startswith("ok") for ln in lines)


def test_key_in_an_app_image_fails(tmp_path, monkeypatch):
    monkeypatch.setattr("check_key_residency.ROOT", tmp_path)
    lines = check([("app", _img(tmp_path, "app.elf", KEY))], KEY)
    assert lines[0].startswith("FAIL")


def test_cm0p_without_key_fails_the_positive_control(tmp_path, monkeypatch):
    monkeypatch.setattr("check_key_residency.ROOT", tmp_path)
    lines = check([("cm0p", _img(tmp_path, "m0.elf", b"\0" * 32))], KEY)
    assert lines[0].startswith("FAIL")


def test_output_never_prints_the_key(tmp_path, monkeypatch):
    monkeypatch.setattr("check_key_residency.ROOT", tmp_path)
    lines = check([("cm0p", _img(tmp_path, "m0.elf", KEY))], KEY)
    assert KEY.hex() not in "".join(lines).lower()
    assert ROOT.is_dir()
