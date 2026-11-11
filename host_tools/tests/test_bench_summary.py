"""test_bench_summary.py -- the bench-test capture summary.

bench_summary is what a bench record's numbers come from (boots, banners per
boot, rejects, lost records, reset causes), so it has to read a real stream the
way logview does: text banners interleaved with 16-byte records.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import log_decode as d      # noqa: E402
import bench_summary as bs           # noqa: E402

BOOT, JUMP, ACCEPT, REJECT_MAC = d.BOOT_EVT_ID, 0x0002, 0x0101, 0x0102


def rec(seq, evt, ts=0, arg0=0, arg1=0, core=0) -> bytes:
    """Mirror of log_rec_encode() in shared/log/src/log.c."""
    body = bytes([d.SYNC, (core << d.CORE_SHIFT) | (seq & d.SEQ_MASK)])
    body += struct.pack("<HIIH", evt, ts, arg0, arg1)
    return body + struct.pack("<H", d.crc16(body))


def gateway_boot(fbl_banners=1) -> bytes:
    """One Node A boot: FBL banner(s), FBL BOOT + jump, app banner, app BOOT."""
    return (b"\n--- fbl nobuildid ---\n" * fbl_banners
            + rec(0, BOOT, arg0=0) + rec(1, JUMP, ts=2466, arg0=0x10040000)
            + b"\n--- app nobuildid ---\n"
            + rec(0, BOOT, arg0=0) + rec(1, ACCEPT, ts=36, arg0=0x200, arg1=1))


def test_jump_event_id_is_the_table_one():
    assert d.log_events.NAME_TO_ID["LOG_EVT_APP_JUMP"] == JUMP


def test_each_boot_is_attributed_to_the_image_whose_banner_precedes_it():
    s = bs.summarize(gateway_boot() + gateway_boot())
    assert [b.image for b in s.boots] == ["fbl", "app", "fbl", "app"]
    assert s.jumps == 2


def test_a_double_banner_is_counted_against_its_boot():
    """F-006: two FBL banners before one BOOT must show, not be merged."""
    s = bs.summarize(gateway_boot(fbl_banners=2) + gateway_boot(fbl_banners=1))
    assert [b.banners for b in s.boots if b.image == "fbl"] == [2, 1]
    assert s.max_banners_per_boot == 2


def test_reset_reason_bits_are_named_for_app_images():
    s = bs.summarize(b"\n--- actuator nobuildid ---\n" + rec(0, BOOT, arg0=0x40010010))
    assert s.boots[0].reset_reason == 0x40010010
    assert bs.reset_bits(0x40010010) == ["PORVDDD", "XRES", "SOFT"]


def test_fbl_reset_reason_is_the_mapped_enum_not_a_bitmask():
    """F-016: the FBL logs fbl_reset_cause_t, so 0 means POWER_ON, not 'no bits'."""
    assert bs.describe_reset("fbl", 0) == "FBL_RST_POWER_ON (mapped enum, F-016)"
    assert bs.describe_reset("actuator", 0x10010) == "XRES | SOFT"


def test_rejects_and_accepts_are_counted_per_can_id():
    data = (b"\n--- app nobuildid ---\n" + rec(0, BOOT)
            + rec(1, ACCEPT, arg0=0x200) + rec(2, ACCEPT, arg0=0x200)
            + rec(3, REJECT_MAC, arg0=0x120, arg1=1))
    s = bs.summarize(data)
    assert s.accepts == {0x200: 2}
    assert s.rejects == {("LOG_EVT_SECOC_REJECT_MAC", 0x120): 1}


def test_lost_records_are_reported():
    data = b"\n--- app nobuildid ---\n" + rec(0, BOOT) + rec(1, ACCEPT) + rec(5, ACCEPT)
    assert bs.summarize(data).lost == 3


def test_report_names_the_numbers_a_bench_record_needs():
    text = bs.format_summary("nodeA", bs.summarize(gateway_boot()))
    for needle in ("boots: 2", "max banners per boot: 1", "lost: 0", "rejects: 0", "jumps: 1"):
        assert needle in text, needle


# ---- LOG_EVT_TASK_STACK (ADR-0025 D5) ---------------------------------------------------

TASK_STACK = 0x0402


def stacks(*pairs) -> bytes:
    """One APP_ALIVE tick's TASK_STACK records: (task_id, free_words) pairs."""
    return b"".join(rec(i, TASK_STACK, arg0=t, arg1=w) for i, (t, w) in enumerate(pairs))


def stack_report(*rows) -> dict:
    """The parts of a stack_bound JSON report the check reads: (entry, required, configured)."""
    return {"image": {"name": "gateway_app.elf", "sha256": "c77007295d3a7147"},
            "tool": {"name": "stack_bound", "version": "0.2.0"},
            "stacks": [{"entry": e, "required": r, "configured": c} for e, r, c in rows]}


def test_task_stack_event_id_is_the_table_one():
    assert d.log_events.NAME_TO_ID["LOG_EVT_TASK_STACK"] == TASK_STACK


def test_the_minimum_free_words_per_task_is_kept_across_ticks_and_boots():
    # @test REQ-LOG-016 : TASK_STACK records are read per task, the minimum kept
    s = bs.summarize(stacks((1, 300), (3, 70)) + gateway_boot() + stacks((1, 280), (3, 90)))
    assert s.min_free == {1: 280, 3: 70}
    assert "task stacks, min free words: can 280, health 70" in bs.format_summary("nodeA", s)


def test_measured_use_within_the_static_bound_passes():
    # can: 2048 - 4*280 = 928 B measured <= 1488 B static
    s = bs.summarize(stacks((1, 280), (6, 100)))
    ok, lines = bs.check_stacks(s, stack_report(("can_task", 1488, 2048)))
    assert ok
    assert any("can_task" in l and "measured   928 B <= static  1488 B" in l and l.endswith("ok") for l in lines)
    assert any(l.strip().startswith("idle") and "no static bound" in l for l in lines)


def test_measured_use_above_the_static_bound_is_a_violation():
    # @test REQ-LOG-016 : measured use above the static bound fails the bench check
    # health: 512 - 4*10 = 472 B measured > 392 B static: the analysis missed a path
    ok, lines = bs.check_stacks(bs.summarize(stacks((3, 10))), stack_report(("health_task", 392, 512)))
    assert not ok and any("VIOLATION" in l for l in lines)


def test_a_task_with_no_records_is_not_a_pass():
    ok, lines = bs.check_stacks(bs.summarize(stacks((1, 280))),
                                stack_report(("can_task", 1488, 2048), ("app_task", 1256, 2048)))
    assert not ok and any("app_task" in l and "NO DATA" in l for l in lines)
