"""Unit tests for secoc_attack's frame crafting (no bus needed)."""
import importlib.util
import pathlib

_PATH = pathlib.Path(__file__).resolve().parents[1] / "secoc_attack" / "secoc_attack.py"
_spec = importlib.util.spec_from_file_location("secoc_attack", _PATH)
atk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(atk)

# A genuine 0x121 from the W40 bench: PDU 00 | epoch 1 | ctr 793 | MAC | 3 B pad
GENUINE = bytes.fromhex("00" "0100" "1903" "52d362cdbe826eb5" "000000")


def test_forged_short_is_a_bare_unlock():
    assert atk.forged_short() == b"\x01"


def test_forged_with_mac_is_padded_like_a_real_frame():
    f = atk.forged_with_mac(bytes(range(12)))
    assert len(f) == 16 and f[0] == 0x01 and f[1:13] == bytes(range(12))


def test_flip_mac_bit_changes_exactly_one_mac_bit():
    flipped = atk.flip_mac_bit(GENUINE)
    diff = [i for i in range(len(GENUINE)) if GENUINE[i] != flipped[i]]
    assert diff == [5]                        # first MAC byte: PDU(1) + freshness(4)
    assert GENUINE[5] ^ flipped[5] == 0x01


def test_counter_of_reads_the_le_freshness_counter():
    assert atk.counter_of(GENUINE) == 793


def test_epoch_of_reads_the_le_freshness_epoch():
    assert atk.epoch_of(GENUINE) == 1


def test_frame_record_round_trips_and_names_the_freshness():
    rec = atk.frame_record(0x121, GENUINE, "2026-10-24T10:00:00")
    assert rec["id"] == "0x121" and rec["epoch"] == 1 and rec["counter"] == 793
    assert atk.frame_from_record(rec) == (0x121, GENUINE)


def test_frame_from_record_refuses_another_id():
    rec = atk.frame_record(0x120, GENUINE, "t")
    try:
        atk.frame_from_record(rec)
    except ValueError:
        return
    raise AssertionError("a 0x120 record must be refused")


def test_frame_from_record_refuses_a_frame_without_trailer():
    rec = {"id": "0x121", "data": "00"}
    try:
        atk.frame_from_record(rec)
    except ValueError:
        return
    raise AssertionError("a frame shorter than PDU + trailer must be refused")


class _FakeBus:
    """Hands out one genuine 0x121, records what is sent."""

    def __init__(self, frames):
        self._frames = list(frames)
        self.sent = []

    def recv(self, timeout=None):
        return self._frames.pop(0) if self._frames else None

    def send(self, msg):
        self.sent.append(msg)


def test_save_then_load_sends_the_saved_bytes_unchanged(tmp_path):
    import can
    path = tmp_path / "bench" / "old_0x121.json"
    genuine = can.Message(arbitration_id=0x121, is_extended_id=False, is_fd=True,
                          data=GENUINE)
    saver = _FakeBus([genuine])
    assert atk.save_genuine(saver, path)
    assert saver.sent == []                    # --save never transmits

    loader = _FakeBus([])
    assert atk.replay_saved(loader, path)
    assert len(loader.sent) == 1
    assert loader.sent[0].arbitration_id == 0x121
    assert bytes(loader.sent[0].data) == GENUINE


def test_save_reports_failure_when_no_genuine_frame(tmp_path, monkeypatch):
    monkeypatch.setattr(atk.time, "monotonic", iter([0.0, 0.0, 99.0]).__next__)
    assert not atk.save_genuine(_FakeBus([]), tmp_path / "x.json")
    assert not (tmp_path / "x.json").exists()
