#!/usr/bin/env python3
"""uds_seam4_probe.py — verify the FBL's real UDS session (M3 Seam 4).

Drives actual UDS requests (0x10 DiagnosticSessionControl, 0x11 ECUReset, and
an unknown-SID negative-response check) against the FBL's diagnostic request
ID and checks the real, service-specific response -- not just an echo, since
request and response now legitimately differ (or share only the SID). Every
message here fits a Single Frame (<=62 bytes), so this script only implements
SF send/receive, not the full FF/CF/FC machinery (already proven separately
by isotp_bringup_probe.py).

Uses this project's own simplified PCI layout (shared/diag/include/
isotp_types.h) -- not standard ISO-TP -- same as isotp_bringup_probe.py.

Run:
    python uds_seam4_probe.py                 # uses ./config.json
    python uds_seam4_probe.py --config other.json
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import can  # python-can >= 4.0

DEFAULT_CONFIG = Path(__file__).with_name("config.json")

PCI_SF = 0x00
UDS_SID_DIAGNOSTIC_SESSION_CONTROL = 0x10
UDS_SID_ECU_RESET = 0x11
UDS_SID_NEGATIVE_RESPONSE = 0x7F
UDS_NRC_SERVICE_NOT_SUPPORTED = 0x11


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


def send_sf(bus: can.BusABC, cfg: dict, arb_id: int, payload: bytes) -> None:
    bus_cfg = cfg["bus"]
    assert len(payload) <= 62, "this probe only sends Single Frames"
    bus.send(can.Message(
        arbitration_id=arb_id,
        is_extended_id=False,
        is_fd=bus_cfg.get("can_fd", True),
        bitrate_switch=bus_cfg.get("bitrate_switch", True),
        data=bytes([PCI_SF, len(payload)]) + payload,
    ))


def recv_sf(bus: can.BusABC, rx_id: int, timeout: float) -> bytes:
    """Wait for a Single Frame on rx_id and return its payload."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"no response on 0x{rx_id:03X}")
        msg = bus.recv(timeout=remaining)
        if msg is None or msg.arbitration_id != rx_id:
            continue
        if (msg.data[0] & 0xF0) != PCI_SF:
            raise RuntimeError(f"expected a Single Frame, got PCI 0x{msg.data[0]:02X}")
        length = msg.data[1]
        return bytes(msg.data[2:2 + length])


def uds_request(bus: can.BusABC, cfg: dict, request: bytes) -> bytes:
    test = cfg["test"]
    send_sf(bus, cfg, parse_id(test["request_id"]), request)
    return recv_sf(bus, parse_id(test["response_id"]), float(test.get("timeout_s", 2.0)))


def run(cfg: dict) -> int:
    try:
        bus = build_bus(cfg)
    except can.CanError as exc:
        app = cfg["interface"].get("app_name", "python-can")
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        print(f"  - is a channel assigned to app '{app}' in Vector Hardware Config?", file=sys.stderr)
        return 2

    checks = []
    try:
        # 1. DiagnosticSessionControl -> programmingSession (0x02)
        resp = uds_request(bus, cfg, bytes([UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x02]))
        ok = resp == bytes([UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x02])
        checks.append(("0x10 progSession -> positive, subfunction echoed", ok, resp))

        # 2. DiagnosticSessionControl -> defaultSession (0x01)
        resp = uds_request(bus, cfg, bytes([UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x01]))
        ok = resp == bytes([UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x01])
        checks.append(("0x10 defaultSession -> positive, subfunction echoed", ok, resp))

        # 3. Unknown SID -> negative response, serviceNotSupported
        resp = uds_request(bus, cfg, bytes([0x99, 0x00]))
        expected = bytes([UDS_SID_NEGATIVE_RESPONSE, 0x99, UDS_NRC_SERVICE_NOT_SUPPORTED])
        ok = resp == expected
        checks.append(("unknown SID 0x99 -> negative, serviceNotSupported", ok, resp))

        # 4. ECUReset (hardReset, 0x01) -- expect the positive response; the FBL
        #    resets immediately after sending it, so nothing further on the bus
        #    from this session (observe the reset itself physically / via the
        #    debugger, e.g. .noinit / the board re-entering the boot decision).
        resp = uds_request(bus, cfg, bytes([UDS_SID_ECU_RESET, 0x01]))
        ok = resp == bytes([UDS_SID_ECU_RESET, 0x01])
        checks.append(("0x11 ECUReset -> positive, subfunction echoed", ok, resp))
    except (TimeoutError, RuntimeError) as exc:
        print(f"FAIL (exception): {exc}")
        checks.append((str(exc), False, b""))
    finally:
        bus.shutdown()

    passed = 0
    for name, ok, resp in checks:
        status = "OK  " if ok else "FAIL"
        print(f"[{status}] {name}: got {resp!r}")
        passed += 1 if ok else 0

    print(f"\n{passed}/{len(checks)} checks passed")
    print("Note: after the ECUReset check, the FBL should have reset -- confirm")
    print("physically (LED behaviour changes) or via the debugger, not by this script.")
    return 0 if passed == len(checks) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the FBL's real UDS session (M3 Seam 4).")
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
