#!/usr/bin/env python3
"""can_sniff.py -- print every frame on the bus, with SecOC frames split open.

A bring-up aid for M5 Stage 5/6 when no CANoe/CANalyzer licence is at hand.
Reuses can_echo_probe's config.json (Vector backend, channel, CAN FD bitrates),
so any interface that works for the echo probe works here (VN1610, VN1611, ...).

    python host_tools/can_sniff/can_sniff.py
    python host_tools/can_sniff/can_sniff.py --out docs/bench/2026-09-30/bus.txt

SecOC frames (ADR-0021) end in freshness(4) || truncated MAC(8); for the known
secured IDs the line shows `payload | fresh | mac` instead of raw bytes.
"""
import argparse
import json
import pathlib
import sys
import time

import can

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE.parent / "can_echo_probe" / "config.json"

TRAILER_LEN = 12   # freshness (4) + truncated MAC (8)
SECURED_IDS = {    # id -> name (secoc-architecture; M5 bring-up plan Stage 5.4)
    0x120: "DOOR_CMD",
    0x121: "LIGHT_CMD",
    0x200: "TELEMETRY",
    0x2F0: "FRESH_SYNC",
}


def format_frame(arb_id: int, data: bytes, t_rel: float) -> str:
    """One line per frame. Pure function, so it is unit-tested without a bus."""
    name = SECURED_IDS.get(arb_id)
    head = f"{t_rel:9.3f}  {arb_id:03X}  len={len(data):2d}"
    if name is not None and len(data) >= TRAILER_LEN:
        payload = data[:-TRAILER_LEN].hex(" ")
        fresh = data[-TRAILER_LEN:-8].hex()
        mac = data[-8:].hex()
        return f"{head}  {name:<10} {payload or '-'} | fresh {fresh} | mac {mac}"
    tag = f"{name:<10} " if name else ""
    return f"{head}  {tag}{data.hex(' ')}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", type=pathlib.Path, default=DEFAULT_CONFIG)
    ap.add_argument("--out", type=pathlib.Path, help="also append lines to this file")
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    itf, bus_cfg = cfg["interface"], cfg["bus"]
    kwargs = dict(
        interface=itf["backend"],
        channel=itf["channel"],
        app_name=itf.get("app_name", "python-can"),
        fd=bus_cfg.get("can_fd", True),
        bitrate=bus_cfg["nominal_bitrate"],
        data_bitrate=bus_cfg["data_bitrate"],
    )
    if itf.get("serial") is not None:
        kwargs["serial"] = itf["serial"]
    try:
        bus = can.Bus(**kwargs)
    except Exception as exc:  # the XL driver raises several types
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        print(f"  - is a channel assigned to app '{kwargs['app_name']}' in Vector Hardware Config?",
              file=sys.stderr)
        return 1

    out = args.out.open("a", encoding="utf-8") if args.out else None
    print("listening (Ctrl+C to stop) ...")
    t0 = time.monotonic()
    try:
        while True:
            msg = bus.recv(timeout=1.0)
            if msg is None:
                continue
            if msg.is_error_frame:
                line = f"{time.monotonic() - t0:9.3f}  ERROR FRAME"
            else:
                line = format_frame(msg.arbitration_id, bytes(msg.data), time.monotonic() - t0)
            print(line, flush=True)
            if out:
                out.write(line + "\n")
                out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
        if out:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
