"""Unit test for can_sniff.format_frame (no bus needed)."""
import importlib.util
import pathlib

_PATH = pathlib.Path(__file__).resolve().parents[1] / "can_sniff" / "can_sniff.py"
_spec = importlib.util.spec_from_file_location("can_sniff", _PATH)
can_sniff = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(can_sniff)


def test_padded_telemetry_splits_at_fixed_offsets():
    # A real Node B frame from the W40 bench: 3 B PDU + epoch 1 + ctr 0x1bf2 + MAC,
    # padded 15 -> 16 B by CAN FD. "Last 12 bytes" would misplace the MAC by one.
    data = bytes.fromhex("000000" "0100" "f21b" "71b9fa4e9d1e45a7" "00")
    line = can_sniff.format_frame(0x200, data, 1.5)
    assert "200  len=16" in line
    assert "TELEMETRY  00 00 00 | epoch 1 ctr  7154 | mac 71b9fa4e9d1e45a7 | pad 00" in line


def test_unpadded_sync_frame_has_no_pad_column():
    data = bytes([0x00, 0x01]) + bytes.fromhex("0100") + bytes.fromhex("0500") + bytes(8)
    line = can_sniff.format_frame(0x2F0, data, 0.0)
    assert "FRESH_SYNC 00 01 | epoch 1 ctr     5 | mac 0000000000000000" in line
    assert "pad" not in line


def test_unsecured_or_short_frames_print_raw():
    assert can_sniff.format_frame(0x123, bytes.fromhex("deadbeef"), 0.0).endswith("de ad be ef")
    # a forged 1-byte DOOR_CMD (Stage 6.1) is too short for a trailer: raw, not split
    assert can_sniff.format_frame(0x120, bytes([0x01]), 0.0).endswith("DOOR_CMD   01")
