"""log_decode.py -- host decoder for the target logging channel (ADR-0023).

Turns the mixed byte stream coming off the KitProg3 UART back into records and
text. The stream is deliberately mixed: LOG_SYNC (0xA5) is outside ASCII, so a
16-byte binary record and a run of plain text share one wire with no escaping
(ADR-0023 D3). That is what lets a boot banner be readable in Tera Term while
the events stay machine-parsable.

Two jobs beyond parsing, both of which exist because the BVT asserts on the
ABSENCE of events:

  * resync -- attaching mid-stream lands mid-record, so a candidate 0xA5 is only
    accepted if its CRC checks out; a bad CRC advances one byte and rescans.
  * loss detection -- a gap in the per-core sequence number proves records were
    dropped. A window with drops cannot support an absence assertion
    (REQ-LOG-007), so the decoder reports gaps rather than quietly closing them.

Wire layout (log_types.h):
    0 sync 0xA5 | 1 core_seq | 2..3 evt | 4..7 ts_ms | 8..11 arg0
                                        | 12..13 arg1 | 14..15 crc16
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

try:
    from . import log_events
except ImportError:                                  # run as a plain script
    import log_events                                # type: ignore

REC_SIZE = 16
SYNC = 0xA5

OFF_CORE_SEQ, OFF_EVT, OFF_TS, OFF_ARG0, OFF_ARG1, OFF_CRC = 1, 2, 4, 8, 12, 14

CORE_SHIFT, SEQ_MASK, CORE_MASK = 6, 0x3F, 0x03
SEQ_MOD = SEQ_MASK + 1

CORE_NAMES = {0: "app", 1: "sec"}

# Resolved from the generated table rather than hardcoded, so it cannot drift
# from events.csv (ADR-0023 D9).
BOOT_EVT_ID = log_events.NAME_TO_ID.get("LOG_EVT_BOOT")

# Argument meanings whose values are addresses: render hex, not decimal.
# "app_entry_addr=268697600" is technically correct and practically useless --
# 0x10040000 is the number you actually compare against the linker map.
_HEX_HINTS = ("addr", "base", "mask", "id")


def _fmt_arg(meaning: str, value: int) -> str:
    lowered = meaning.lower()
    if any(h in lowered for h in _HEX_HINTS):
        return f"{meaning}=0x{value:X}"
    return f"{meaning}={value}"


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.

    Must match log_crc16() in shared/log/src/log.c byte for byte -- the pair is
    pinned by test_log_decode against the canonical check value 0x29B1.
    """
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Record:
    core: int
    seq: int
    evt: int
    ts_ms: int
    arg0: int
    arg1: int

    @property
    def core_name(self) -> str:
        return CORE_NAMES.get(self.core, f"core{self.core}")

    @property
    def name(self) -> str:
        return log_events.name(self.evt)

    @property
    def is_contract(self) -> bool:
        """True if a test may assert on this event (REQ-LOG-008)."""
        return log_events.is_contract(self.evt)

    def __str__(self) -> str:
        meta = log_events.EVENTS.get(self.evt)
        if meta:
            _, _, a0, a1, _ = meta
            args = []
            if a0 != "-":
                args.append(_fmt_arg(a0, self.arg0))
            if a1 != "-":
                args.append(_fmt_arg(a1, self.arg1))
            tail = " ".join(args)
        else:
            tail = f"arg0={self.arg0} arg1={self.arg1}"
        return f"[{self.ts_ms:>8} {self.core_name}] {self.name} {tail}".rstrip()


@dataclass
class Text:
    text: str

    def __str__(self) -> str:
        return self.text


@dataclass
class Gap:
    """Records were lost between two observed sequence numbers (REQ-LOG-006/007)."""
    core: int
    missing: int

    def __str__(self) -> str:
        core = CORE_NAMES.get(self.core, f"core{self.core}")
        return f"*** {core}: {self.missing} record(s) LOST (sequence gap) ***"


class Decoder:
    """Incremental decoder. Feed bytes, get items out.

    Incremental because the live view reads whatever the serial port has; the
    decoder must tolerate a record split across two reads.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._last_seq: dict[int, int] = {}
        self.records = 0
        self.text_bytes = 0
        self.resyncs = 0
        self.lost = 0

    def feed(self, data: bytes) -> list:
        self._buf.extend(data)
        return self._drain()

    def flush(self) -> list:
        """Emit any trailing text. Call at end of a file/capture.

        Trailing BINARY is not emitted: a partial record at the end is more
        likely a truncated capture than a real record, and inventing one would
        be exactly the false evidence this channel exists to avoid.
        """
        out = []
        if self._buf and SYNC not in self._buf:
            out.append(Text(self._buf.decode("ascii", errors="replace")))
            self.text_bytes += len(self._buf)
            self._buf.clear()
        return out

    # ------------------------------------------------------------------
    def _drain(self) -> list:
        out: list = []
        while True:
            idx = self._buf.find(SYNC)

            if idx == -1:                       # no record start in sight: all text
                if self._buf:
                    out.append(Text(self._buf.decode("ascii", errors="replace")))
                    self.text_bytes += len(self._buf)
                    self._buf.clear()
                return out

            if idx > 0:                         # text before the next record
                chunk = bytes(self._buf[:idx])
                out.append(Text(chunk.decode("ascii", errors="replace")))
                self.text_bytes += len(chunk)
                del self._buf[:idx]

            if len(self._buf) < REC_SIZE:       # need more bytes to decide
                return out

            frame = bytes(self._buf[:REC_SIZE])
            if crc16(frame[:OFF_CRC]) != struct.unpack_from("<H", frame, OFF_CRC)[0]:
                # Not a real record -- a payload byte happened to be 0xA5, or we
                # attached mid-record. Drop ONE byte and rescan; skipping the
                # whole 16 would step over a genuine record start.
                self.resyncs += 1
                self._buf.pop(0)
                continue

            out.extend(self._emit(frame))
            del self._buf[:REC_SIZE]

    def _emit(self, frame: bytes) -> list:
        core_seq = frame[OFF_CORE_SEQ]
        core = (core_seq >> CORE_SHIFT) & CORE_MASK
        seq = core_seq & SEQ_MASK
        evt_id = struct.unpack_from("<H", frame, OFF_EVT)[0]

        out: list = []
        prev = self._last_seq.get(core)

        # LOG_EVT_BOOT means the producer restarted: log_init() zeroed its
        # sequence counter, so a "gap" here is an artefact of the reset, not
        # lost records. Reporting it would be a FALSE positive -- and under
        # REQ-LOG-007 a consumer must fail any window containing loss, so a
        # false gap fails a good run. The BVT power-cycles between tests and
        # spans the FBL->app handover, so this fires constantly if unhandled.
        if evt_id == BOOT_EVT_ID:
            prev = None

        if prev is not None:
            missing = (seq - prev - 1) % SEQ_MOD
            if missing:
                self.lost += missing
                out.append(Gap(core, missing))
        self._last_seq[core] = seq

        evt, ts, arg0, arg1 = (
            struct.unpack_from("<H", frame, OFF_EVT)[0],
            struct.unpack_from("<I", frame, OFF_TS)[0],
            struct.unpack_from("<I", frame, OFF_ARG0)[0],
            struct.unpack_from("<H", frame, OFF_ARG1)[0],
        )
        self.records += 1
        out.append(Record(core, seq, evt, ts, arg0, arg1))
        return out


def decode_all(data: bytes) -> list:
    """Convenience for a complete capture (a file, or a BVT test window)."""
    d = Decoder()
    items = d.feed(data)
    items.extend(d.flush())
    return items
