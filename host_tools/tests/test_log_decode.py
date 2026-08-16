"""test_log_decode.py -- host decoder contract (REQ-LOG-013, ADR-0023 D8).

The point of this file: a format drift between shared/log/src/log.c and
host_tools/lib/log_decode.py must fail HERE, on a laptop, in a second -- not on
the bench at midnight, and above all not silently. A drifted decoder does not
crash; it renders plausible wrong values, which would make the BVT report
confident nonsense.

The C encoder is the reference. These tests pin the same layout and the same
CRC from the Python side, plus the two behaviours the BVT depends on: mid-stream
resync, and sequence-gap (record loss) detection.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import log_decode as d      # noqa: E402
from lib import log_events           # noqa: E402


def encode(core=0, seq=0, evt=0x0401, ts=0, arg0=0, arg1=0) -> bytes:
    """Mirror of log_rec_encode() in shared/log/src/log.c."""
    body = bytes([d.SYNC, (core << d.CORE_SHIFT) | (seq & d.SEQ_MASK)])
    body += struct.pack("<HIIH", evt, ts, arg0, arg1)
    return body + struct.pack("<H", d.crc16(body))


# --- CRC ------------------------------------------------------------------

def test_crc_matches_the_canonical_check_value():
    """CRC-16/CCITT-FALSE("123456789") == 0x29B1.

    Same constant test_log.c pins on the C side. If both sides assert against
    the published check value, neither can drift without one of them failing.
    """
    assert d.crc16(b"123456789") == 0x29B1


# --- layout ---------------------------------------------------------------

def test_record_round_trips():
    rec = d.decode_all(encode(core=1, seq=5, evt=0x0102, ts=0xDEADBEEF,
                              arg0=0x120, arg1=0x1234))
    assert len(rec) == 1
    r = rec[0]
    assert (r.core, r.seq, r.evt) == (1, 5, 0x0102)
    assert (r.ts_ms, r.arg0, r.arg1) == (0xDEADBEEF, 0x120, 0x1234)
    assert r.core_name == "sec"
    assert r.name == "LOG_EVT_SECOC_REJECT_MAC"


def test_record_size_matches_the_c_side():
    assert len(encode()) == d.REC_SIZE == 16


def test_corrupt_record_is_not_decoded():
    """A flipped byte must fail the CRC, not produce a plausible record."""
    bad = bytearray(encode(arg0=1))
    bad[d.OFF_ARG0] ^= 0xFF
    assert not [i for i in d.decode_all(bytes(bad)) if isinstance(i, d.Record)]


# --- text passthrough (ADR-0023 D3) ---------------------------------------

def test_text_and_records_share_the_stream():
    """The boot banner is readable in a terminal AND parsable here."""
    items = d.decode_all(b"traveo-body app build=unknown\n" + encode(evt=0x0401))
    assert isinstance(items[0], d.Text)
    assert "traveo-body app" in items[0].text
    assert isinstance(items[1], d.Record)


def test_text_only_stream_needs_no_records():
    items = d.decode_all(b"hello\n")
    assert len(items) == 1 and isinstance(items[0], d.Text)


# --- resync ---------------------------------------------------------------

def test_attaching_mid_record_resyncs():
    """Opening the port mid-record is the normal case, not an edge case."""
    stream = encode(seq=1, arg0=111) + encode(seq=2, arg0=222)
    items = d.decode_all(stream[7:])            # start inside the first record
    recs = [i for i in items if isinstance(i, d.Record)]
    assert [r.arg0 for r in recs] == [222]


def test_sync_byte_inside_a_payload_does_not_derail():
    """0xA5 can legitimately appear in ts/arg bytes; only the CRC decides."""
    stream = encode(seq=1, arg0=0xA5A5A5A5) + encode(seq=2, arg0=7)
    recs = [i for i in d.decode_all(stream) if isinstance(i, d.Record)]
    assert [r.arg0 for r in recs] == [0xA5A5A5A5, 7]


# --- loss detection (REQ-LOG-006/007) -------------------------------------

def test_sequence_gap_is_reported():
    """The BVT asserts on the ABSENCE of events. 'Absent' and 'dropped' must be
    distinguishable, or a replay test passes when the node was merely lossy."""
    items = d.decode_all(encode(seq=1) + encode(seq=4))
    gaps = [i for i in items if isinstance(i, d.Gap)]
    assert len(gaps) == 1 and gaps[0].missing == 2


def test_sequence_wrap_is_not_a_gap():
    """seq is 6 bits; 63 -> 0 is normal, not loss."""
    items = d.decode_all(encode(seq=63) + encode(seq=0))
    assert not [i for i in items if isinstance(i, d.Gap)]


def test_boot_event_resets_sequence_tracking():
    """A restart is not record loss.

    Observed on hardware: the FBL logs, jumps, and the app calls log_init()
    which zeroes the sequence counter. The decoder saw seq go backwards and
    reported "62 records LOST" for a run that lost nothing. Under REQ-LOG-007 a
    consumer must FAIL any window containing loss — so a false gap fails a
    perfectly good BVT run, and the BVT power-cycles between every test.
    """
    boot = d.log_events.NAME_TO_ID["LOG_EVT_BOOT"]
    items = d.decode_all(encode(seq=40) + encode(seq=0, evt=boot) + encode(seq=1))
    assert not [i for i in items if isinstance(i, d.Gap)]


def test_boot_event_does_not_mask_a_real_gap_after_it():
    """Resetting on BOOT must not blind the decoder to genuine loss later."""
    boot = d.log_events.NAME_TO_ID["LOG_EVT_BOOT"]
    items = d.decode_all(encode(seq=0, evt=boot) + encode(seq=5))
    gaps = [i for i in items if isinstance(i, d.Gap)]
    assert len(gaps) == 1 and gaps[0].missing == 4


def test_address_arguments_render_in_hex():
    """"app_entry_addr=268697600" is correct and useless; 0x10040000 is the
    number you compare against the linker map."""
    jump = d.log_events.NAME_TO_ID["LOG_EVT_APP_JUMP"]
    line = str(d.decode_all(encode(evt=jump, arg0=0x10040000))[0])
    assert "0x10040000" in line


def test_plain_counters_stay_decimal():
    alive = d.log_events.NAME_TO_ID["LOG_EVT_APP_ALIVE"]
    line = str(d.decode_all(encode(evt=alive, arg0=1500))[0])
    assert "uptime_ms=1500" in line


def test_cores_have_independent_sequences():
    """Interleaved cores must not look like loss to each other."""
    items = d.decode_all(encode(core=0, seq=1) + encode(core=1, seq=40)
                         + encode(core=0, seq=2) + encode(core=1, seq=41))
    assert not [i for i in items if isinstance(i, d.Gap)]


# --- incremental feed -----------------------------------------------------

def test_record_split_across_reads():
    """A serial read can end mid-record; the decoder must hold and resume."""
    dec = d.Decoder()
    blob = encode(arg0=42)
    assert not [i for i in dec.feed(blob[:9]) if isinstance(i, d.Record)]
    recs = [i for i in dec.feed(blob[9:]) if isinstance(i, d.Record)]
    assert len(recs) == 1 and recs[0].arg0 == 42


def test_trailing_partial_record_is_not_invented():
    """A truncated capture must lose the partial record, never fabricate one."""
    dec = d.Decoder()
    dec.feed(encode()[:10])
    assert not [i for i in dec.flush() if isinstance(i, d.Record)]


# --- event table (REQ-LOG-008/012) ----------------------------------------

def test_contract_and_diagnostic_ranges_agree_with_the_table():
    for evt_id, (name, cls, _, _, _) in log_events.EVENTS.items():
        assert log_events.is_contract(evt_id) == (cls == "contract"), name


def test_unknown_event_id_renders_without_crashing():
    """A decoder older than the firmware must degrade, not die."""
    r = d.decode_all(encode(evt=0x0EEE))[0]
    assert r.name.lower() == "evt_0x0eee"
    assert r.is_contract          # range still classifies it, name or not
