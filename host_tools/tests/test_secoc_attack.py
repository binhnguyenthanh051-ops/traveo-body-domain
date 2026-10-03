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
