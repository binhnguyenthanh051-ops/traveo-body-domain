#!/usr/bin/env python3
"""isotp_bringup_probe.py — verify the FBL's ISO-TP echo (M3 Seam 2 bring-up).

Sends a message to the FBL's diagnostic request ID, and reads back its echo on
the response ID, using this project's own SIMPLIFIED PCI layout -- NOT
standard ISO 15765-2 (see shared/diag/include/isotp_types.h for the exact
byte layout and the honesty note on why it's simplified). Standard tools
(python-can-isotp, CANoe, ...) will NOT interoperate with this -- that's a
deliberate scope choice for M3, not an oversight.

Handles both directions: sending a (possibly segmented) request and waiting
for our own flow-control CTS, AND receiving the FBL's (possibly segmented)
echo and sending flow control back to it -- the FBL's port_prog.c bring-up
loop (run_isotp_bringup_echo) round-trips whatever it reassembles, so a long
enough message exercises FF/CF/FC in both directions in one probe.

Run:
    python isotp_bringup_probe.py                 # uses ./config.json
    python isotp_bringup_probe.py --config other.json
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import can  # python-can >= 4.0

DEFAULT_CONFIG = Path(__file__).with_name("config.json")

# PCI layout (isotp_types.h) -- top nibble of byte 0 is the frame type.
PCI_SF = 0x00
PCI_FF = 0x10
PCI_CF = 0x20
PCI_FC = 0x30

FC_CTS = 0x00
FC_WAIT = 0x01
FC_OVERFLOW = 0x02

FF_INITIAL_LEN = 59   # isotp.c's ISOTP_FF_INITIAL_LEN (5-byte header + 59 = 64, a valid CAN FD length)


def parse_id(value: object) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def load_config(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def build_bus(cfg: dict) -> can.BusABC:
    itf = cfg["interface"]
    bus = cfg["bus"]
    kwargs = dict(
        interface=itf["backend"],
        channel=itf["channel"],
        app_name=itf.get("app_name", "python-can"),
        fd=bus.get("can_fd", True),
        bitrate=bus["nominal_bitrate"],
        data_bitrate=bus["data_bitrate"],
        receive_own_messages=False,
    )
    if itf.get("serial") is not None:
        kwargs["serial"] = itf["serial"]
    return can.Bus(**kwargs)


def send_frame(bus: can.BusABC, cfg: dict, arb_id: int, data: bytes) -> None:
    bus_cfg = cfg["bus"]
    bus.send(can.Message(
        arbitration_id=arb_id,
        is_extended_id=False,
        is_fd=bus_cfg.get("can_fd", True),
        bitrate_switch=bus_cfg.get("bitrate_switch", True),
        data=data,
    ))


def recv_matching(bus: can.BusABC, arb_id: int, deadline: float) -> can.Message | None:
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        msg = bus.recv(timeout=remaining)
        if msg is not None and msg.arbitration_id == arb_id:
            return msg


def isotp_send(bus: can.BusABC, cfg: dict, tx_id: int, rx_id_for_fc: int,
                payload: bytes, timeout: float) -> None:
    """Send payload to tx_id, as SF or FF + (wait for FC) + CF burst."""
    if len(payload) <= 62:
        send_frame(bus, cfg, tx_id, bytes([PCI_SF, len(payload)]) + payload)
        return

    initial = payload[:FF_INITIAL_LEN]
    rest = payload[FF_INITIAL_LEN:]
    header = bytes([PCI_FF]) + len(payload).to_bytes(4, "big")
    send_frame(bus, cfg, tx_id, header + initial)

    fc = recv_matching(bus, rx_id_for_fc, time.monotonic() + timeout)
    if fc is None or (fc.data[0] & 0xF0) != PCI_FC:
        raise TimeoutError(f"no flow control from 0x{rx_id_for_fc:03X}")
    status = fc.data[0] & 0x0F
    if status != FC_CTS:
        raise RuntimeError(f"flow control status 0x{status:02X}, expected CTS")

    seq = 1
    while rest:
        chunk, rest = rest[:63], rest[63:]
        send_frame(bus, cfg, tx_id, bytes([PCI_CF | (seq & 0x0F)]) + chunk)
        seq += 1


def isotp_recv(bus: can.BusABC, cfg: dict, rx_id: int, tx_id_for_fc: int,
                timeout: float) -> bytes:
    """Receive a (possibly segmented) message on rx_id, sending flow control
    back to tx_id_for_fc if it's multi-frame."""
    deadline = time.monotonic() + timeout
    first = recv_matching(bus, rx_id, deadline)
    if first is None:
        raise TimeoutError(f"no frame received on 0x{rx_id:03X}")

    pci_type = first.data[0] & 0xF0
    if pci_type == PCI_SF:
        length = first.data[1]
        return bytes(first.data[2:2 + length])

    if pci_type != PCI_FF:
        raise RuntimeError(f"expected SF or FF, got PCI 0x{first.data[0]:02X}")

    total_len = int.from_bytes(first.data[1:5], "big")
    buf = bytearray(first.data[5:])
    send_frame(bus, cfg, tx_id_for_fc, bytes([PCI_FC | FC_CTS, 8, 0]))

    expected_seq = 1
    while len(buf) < total_len:
        cf = recv_matching(bus, rx_id, deadline)
        if cf is None:
            raise TimeoutError("timed out waiting for a consecutive frame")
        if (cf.data[0] & 0xF0) != PCI_CF:
            raise RuntimeError(f"expected CF, got PCI 0x{cf.data[0]:02X}")
        seq = cf.data[0] & 0x0F
        if seq != (expected_seq & 0x0F):
            raise RuntimeError(f"wrong sequence number: got {seq}, expected {expected_seq & 0x0F}")
        # Take only what's still logically needed, not the whole frame -- CAN
        # FD only has specific DLC-representable lengths (0-8, then 12/16/
        # 20/24/32/48/64), so a CF whose real payload doesn't land exactly on
        # one gets padded on the wire. Trusting cf.data's full length here
        # would capture that padding as message content (the exact bug this
        # tool caught on the FBL side, fixed in isotp.c's handle_cf()).
        needed = total_len - len(buf)
        buf.extend(cf.data[1:1 + needed])
        expected_seq += 1

    return bytes(buf[:total_len])


def run(cfg: dict) -> int:
    test = cfg["test"]
    request_id = parse_id(test["request_id"])
    response_id = parse_id(test["response_id"])
    timeout = float(test.get("timeout_s", 2.0))
    messages = test.get("messages", ["hello"])

    try:
        bus = build_bus(cfg)
    except can.CanError as exc:
        app = cfg["interface"].get("app_name", "python-can")
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        print(f"  - is a channel assigned to app '{app}' in Vector Hardware Config?", file=sys.stderr)
        return 2

    passed = 0
    try:
        for i, text in enumerate(messages):
            payload = text.encode("ascii")
            print(f"[{i}] sending {len(payload)} bytes to 0x{request_id:03X} ...")
            try:
                isotp_send(bus, cfg, request_id, response_id, payload, timeout)
                echoed = isotp_recv(bus, cfg, response_id, request_id, timeout)
            except (TimeoutError, RuntimeError) as exc:
                print(f"    FAIL: {exc}")
                continue

            if echoed == payload:
                passed += 1
                print(f"    OK: echo matched ({len(echoed)} bytes)")
            else:
                print(f"    FAIL: echo mismatch -- sent {payload!r}, got {echoed!r}")
    finally:
        bus.shutdown()

    print(f"\n{passed}/{len(messages)} messages echoed correctly")
    return 0 if passed == len(messages) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the FBL's ISO-TP echo (M3 Seam 2).")
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
