#!/usr/bin/env python3
"""can_echo_probe.py — verify the Node A gateway's CAN FD echo (M2 seam 2, Phase B).

Drives a Vector VN1610 (or any python-can-supported CAN FD interface) to prove the
gateway's RX->task->TX echo on the real bus. NO CANalyzer license required — only
the free Vector XL driver + python-can.

Each iteration it (1) sends a CAN FD frame with a known id + payload, (2) waits for
the gateway to echo the *same* frame back, and (3) by being on the bus, provides the
ACK the single gateway node needs. All settings live in config.json (next to this
file); see README.md.

Run:
    python can_echo_probe.py                 # uses ./config.json
    python can_echo_probe.py --config other.json
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import can  # python-can >= 4.0

# Body-domain CAN IDs — mirror shared/messages/body_msgs.h / ADR-0002.
MSG_ID_SENSOR_RPT = 0x200

DEFAULT_CONFIG = Path(__file__).with_name("config.json")


def parse_id(value: object) -> int:
    """Accept a CAN id as an int or a hex/dec string (e.g. "0x200")."""
    return int(value, 0) if isinstance(value, str) else int(value)


def load_config(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def build_bus(cfg: dict) -> can.BusABC:
    """Open the interface in CAN FD mode (nominal + data bitrate) from config."""
    itf = cfg["interface"]
    bus = cfg["bus"]
    kwargs = dict(
        interface=itf["backend"],
        channel=itf["channel"],
        app_name=itf.get("app_name", "python-can"),
        fd=bus.get("can_fd", True),
        bitrate=bus["nominal_bitrate"],
        data_bitrate=bus["data_bitrate"],
        receive_own_messages=False,   # a match can then only be the gateway's echo
    )
    if itf.get("serial") is not None:
        kwargs["serial"] = itf["serial"]   # select a specific device by serial number
    return can.Bus(**kwargs)


def make_frame(cfg: dict, arb_id: int, payload: bytes) -> can.Message:
    bus = cfg["bus"]
    return can.Message(
        arbitration_id=arb_id,
        is_extended_id=False,
        is_fd=bus.get("can_fd", True),
        bitrate_switch=bus.get("bitrate_switch", True),
        data=payload,
    )


def wait_for_echo(bus: can.BusABC, arb_id: int, payload: bytes, timeout: float) -> can.Message | None:
    """Return the first frame matching id+payload within `timeout`, else None."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        msg = bus.recv(timeout=remaining)
        if msg is None:
            return None
        if msg.arbitration_id == arb_id and bytes(msg.data) == payload:
            return msg


def run(cfg: dict) -> int:
    test = cfg["test"]
    tx_id = parse_id(test["tx_id"])
    count = int(test.get("count", 10))
    timeout = float(test.get("timeout_s", 1.0))
    interval = float(test.get("interval_s", 0.2))

    try:
        bus = build_bus(cfg)
    except can.CanError as exc:
        app = cfg["interface"].get("app_name", "python-can")
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        print("  - is the Vector XL driver installed and the interface plugged in?", file=sys.stderr)
        print(f"  - is a channel assigned to app '{app}' in Vector Hardware Config?", file=sys.stderr)
        return 2

    passed = 0
    try:
        for i in range(count):
            # sensor report (0x200): ambient_raw=0x0064 big-endian, door_ajar toggles.
            payload = bytes([0x00, 0x64, i & 1]) if tx_id == MSG_ID_SENSOR_RPT else bytes([i & 0xFF])
            bus.send(make_frame(cfg, tx_id, payload))
            echo = wait_for_echo(bus, tx_id, payload, timeout)
            if echo is not None:
                passed += 1
                print(f"[{i:2d}] echo OK   id=0x{tx_id:03X} data={payload.hex(' ')}")
            else:
                print(f"[{i:2d}] NO ECHO  id=0x{tx_id:03X} data={payload.hex(' ')} "
                      f"(timeout {timeout:.1f}s)")
            time.sleep(interval)
    finally:
        bus.shutdown()

    print(f"\n{passed}/{count} echoes received")
    return 0 if passed == count else 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the Node A gateway CAN FD echo (Phase B).")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                        help="path to the JSON config (default: ./config.json)")
    args = parser.parse_args()

    try:
        cfg = load_config(args.config)
    except FileNotFoundError:
        print(f"error: config not found: {args.config}", file=sys.stderr)
        sys.exit(2)
    except json.JSONDecodeError as exc:
        print(f"error: invalid JSON in {args.config}: {exc}", file=sys.stderr)
        sys.exit(2)

    sys.exit(run(cfg))


if __name__ == "__main__":
    main()
