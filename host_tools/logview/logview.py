#!/usr/bin/env python3
"""logview.py -- read and render the target log stream (ADR-0023).

Replaces Tera Term once the firmware starts emitting binary records: text still
comes through verbatim (the banner stays readable), and 16-byte records are
rendered as named events with their arguments.

    python host_tools/logview/logview.py --port COM7
    python host_tools/logview/logview.py --file capture.bin
    python host_tools/logview/logview.py --port COM7 --raw capture.bin

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


def from_file(path: Path, out, colour: bool) -> int:
    data = path.read_bytes()
    dec = log_decode.Decoder()
    items = dec.feed(data)
    items.extend(dec.flush())
    render(items, out, colour)
    summarise(dec, out)
    return 0


def from_serial(port: str, baud: int, raw: Path | None, out, colour: bool) -> int:
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
                render(dec.feed(chunk), out, colour)
    except KeyboardInterrupt:
        render(dec.flush(), out, colour)
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
    args = ap.parse_args()

    colour = not args.no_colour and sys.stdout.isatty()
    if args.file:
        return from_file(args.file, sys.stdout, colour)
    return from_serial(args.port, args.baud, args.raw, sys.stdout, colour)


if __name__ == "__main__":
    sys.exit(main())
