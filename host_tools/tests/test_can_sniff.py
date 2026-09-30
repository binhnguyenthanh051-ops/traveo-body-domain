"""Unit test for can_sniff.format_frame (no bus needed)."""
import importlib.util
import pathlib

_PATH = pathlib.Path(__file__).resolve().parents[1] / "can_sniff" / "can_sniff.py"
_spec = importlib.util.spec_from_file_location("can_sniff", _PATH)
can_sniff = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(can_sniff)


def test_secured_frame_is_split_into_payload_freshness_mac():
    data = bytes([0x00, 0x64, 0x01]) + bytes.fromhex("00000007") + bytes.fromhex("1122334455667788")
    line = can_sniff.format_frame(0x200, data, 1.5)
    assert "200  len=15" in line
    assert "TELEMETRY" in line
    assert "00 64 01 | fresh 00000007 | mac 1122334455667788" in line


def test_sync_frame_with_two_byte_payload():
    data = bytes([0x00, 0x01]) + bytes(4) + bytes(8)
    assert "FRESH_SYNC 00 01 | fresh 00000000" in can_sniff.format_frame(0x2F0, data, 0.0)


def test_unsecured_or_short_frames_print_raw():
    assert can_sniff.format_frame(0x123, bytes.fromhex("deadbeef"), 0.0).endswith("de ad be ef")
    # a forged 1-byte DOOR_CMD (Stage 6.1) is too short for a trailer: raw, not split
    assert can_sniff.format_frame(0x120, bytes([0x01]), 0.0).endswith("DOOR_CMD   01")
