#!/usr/bin/env python3
"""logview.py -- read and render the target log stream (ADR-0023).

Replaces Tera Term once the firmware starts emitting binary records: text still
comes through verbatim (the banner stays readable), and 16-byte records are
rendered as named events with their arguments.

    python host_tools/logview/logview.py --port COM7
    python host_tools/logview/logview.py --file capture.bin
    python host_tools/logview/logview.py --port COM7 --raw capture.bin
    python host_tools/logview/logview.py --file capture.bin --all

SECOC_ACCEPT (one per secured frame, ~50/s on Node A) is folded by default into
one line per CAN ID every --fold-ms of target time. Rejects, resyncs, gaps and
text always print as they arrive. Use --all for a transcript that is evidence
(every frame, e.g. "continuous counters"); the --raw capture always has them all.

Keep a --raw capture whenever you are diagnosing something: it is the artefact
the BVT uploads on failure, and it can be replayed through --file afterwards.

Serial needs pyserial (`pip install pyserial`); --file needs nothing.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import log_decode  # noqa: E402

DEFAULT_BAUD = 1000000      # ADR-0023: 1 Mbps, not the bridge's 4 Mbps ceiling
DEFAULT_FOLD_MS = 5000      # one ACCEPT summary line per CAN ID per window

ACCEPT_EVT_ID = log_decode.log_events.NAME_TO_ID["LOG_EVT_SECOC_ACCEPT"]


class AcceptSummary:
    """Stands in for a run of SECOC_ACCEPT records from one core + CAN ID."""

    def __init__(self, first: log_decode.Record):
        self.core_name = first.core_name
        self.can_id = first.arg0
        self.count = 0
        self.first_ts = first.ts_ms
        self.first_ctr = first.arg1
        self.last_ts = first.ts_ms
        self.last_ctr = first.arg1

    def add(self, r: log_decode.Record) -> None:
        self.count += 1
        self.last_ts = r.ts_ms
        self.last_ctr = r.arg1

    def __str__(self) -> str:
        return (f"[{self.last_ts:>8} {self.core_name}] LOG_EVT_SECOC_ACCEPT x{self.count} "
                f"can_id=0x{self.can_id:03X} freshness_ctr={self.first_ctr}..{self.last_ctr} "
                f"over {self.last_ts - self.first_ts} ms (folded)")


class AcceptFolder:
    """Display-only folding of SECOC_ACCEPT, one record per secured frame (REQ-LOG-009).

    The firmware keeps emitting every ACCEPT (the BVT and the --raw capture need
    them); only the console view folds them. ONLY accepts fold: rejects, resyncs,
    gaps and text pass straight through, and anything that can mean a reboot
    (text such as a banner, a gap, LOG_EVT_BOOT, a timestamp going backwards)
    first flushes what is pending, so no summary straddles a restart.
    """

    def __init__(self, window_ms: int = DEFAULT_FOLD_MS):
        self.window_ms = window_ms
        self._pending: dict[tuple[int, int], AcceptSummary] = {}
        self._window_start: int | None = None
        self._last_ts: int | None = None

    def feed(self, items) -> list:
        out: list = []
        for item in items:
            if isinstance(item, log_decode.Record):
                restarted = (item.evt == log_decode.BOOT_EVT_ID
                             or (self._last_ts is not None and item.ts_ms < self._last_ts))
                expired = (self._window_start is not None
                           and item.ts_ms - self._window_start >= self.window_ms)
                if restarted or expired:
                    out.extend(self.flush())
                self._last_ts = item.ts_ms
                if item.evt == ACCEPT_EVT_ID:
                    if self._window_start is None:
                        self._window_start = item.ts_ms
                    key = (item.core, item.arg0)
                    if key not in self._pending:
                        self._pending[key] = AcceptSummary(item)
                    self._pending[key].add(item)
                    continue
            else:
                out.extend(self.flush())
            out.append(item)
        return out

    def flush(self) -> list:
        out = list(self._pending.values())
        self._pending.clear()
        self._window_start = None
        return out


def render(items, out, colour: bool) -> None:
    for item in items:
        if isinstance(item, log_decode.Text):
            out.write(item.text)
        elif isinstance(item, log_decode.Gap):
            # Loud on purpose: a window containing a gap cannot support an
            # absence assertion (REQ-LOG-007). Never let this scroll by quietly.
            line = str(item)
            out.write(f"\033[1;31m{line}\033[0m\n" if colour else f"{line}\n")
        else:
            out.write(f"{item}\n")
    out.flush()


def view(items, folder: AcceptFolder | None) -> list:
    return folder.feed(items) if folder else items


def from_file(path: Path, out, colour: bool, folder: AcceptFolder | None) -> int:
    data = path.read_bytes()
    dec = log_decode.Decoder()
    items = dec.feed(data)
    items.extend(dec.flush())
    render(view(items, folder), out, colour)
    if folder:
        render(folder.flush(), out, colour)
    summarise(dec, out)
    return 0


def from_serial(port: str, baud: int, raw: Path | None, out, colour: bool,
                folder: AcceptFolder | None) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial not installed:  pip install pyserial", file=sys.stderr)
        return 2

    dec = log_decode.Decoder()
    raw_fh = None
    if raw:
        Path(raw).parent.mkdir(parents=True, exist_ok=True)   # e.g. a new docs/bench/<date>/
        raw_fh = open(raw, "wb")

    try:
        with serial.Serial(port, baud, timeout=0.2) as ser:
            print(f"# {port} @ {baud} 8N1 -- Ctrl-C to stop", file=sys.stderr)
            while True:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                if raw_fh:
                    raw_fh.write(chunk)
                    raw_fh.flush()
                render(view(dec.feed(chunk), folder), out, colour)
    except KeyboardInterrupt:
        render(view(dec.flush(), folder), out, colour)
        if folder:
            render(folder.flush(), out, colour)
        summarise(dec, out)
        return 0
    except Exception as exc:                      # noqa: BLE001 - report and exit
        print(f"serial error: {exc}", file=sys.stderr)
        return 1
    finally:
        if raw_fh:
            raw_fh.close()


def summarise(dec: log_decode.Decoder, out) -> None:
    out.write(
        f"\n# {dec.records} records, {dec.text_bytes} text bytes, "
        f"{dec.resyncs} resync(s), {dec.lost} record(s) lost\n"
    )
    if dec.lost:
        out.write("# WARNING: records were LOST -- this window cannot support an\n"
                  "#          absence assertion (REQ-LOG-007).\n")
    out.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyACM0")
    src.add_argument("--file", type=Path, help="decode a saved capture instead")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--raw", type=Path, help="also write the raw stream here (serial only)")
    ap.add_argument("--no-colour", action="store_true")
    ap.add_argument("--all", action="store_true",
                    help="print every SECOC_ACCEPT instead of folding them (use for evidence transcripts)")
    ap.add_argument("--fold-ms", type=int, default=DEFAULT_FOLD_MS,
                    help=f"ACCEPT summary window in target ms (default {DEFAULT_FOLD_MS})")
    args = ap.parse_args()

    # F-005: a redirected stdout on Windows is cp1252, which can't encode the
    # U+FFFD the decoder substitutes for a non-ASCII byte. Always write UTF-8.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    colour = not args.no_colour and sys.stdout.isatty()
    folder = None if args.all else AcceptFolder(args.fold_ms)
    if args.file:
        return from_file(args.file, sys.stdout, colour, folder)
    return from_serial(args.port, args.baud, args.raw, sys.stdout, colour, folder)


if __name__ == "__main__":
    sys.exit(main())
