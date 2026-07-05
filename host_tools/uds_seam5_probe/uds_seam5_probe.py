#!/usr/bin/env python3
"""uds_seam5_probe.py — verify the FBL's real SecurityAccess round trip
(M3 Seam 5): 0x27 requestSeed / sendKey, correct-key unlock, wrong-key
rejection + relock, and the sequence-error cases.

The seed/key transform (seed ^ UDS_SECURITY_KEY_XOR_CONST) is deliberately
simple and NOT a secret (shared/diag/include/uds_security_access.h's honesty
note: mechanism, not real security) -- a real PC tool would need to know it
the same way this script does.

Every message here fits a Single Frame. Uses this project's own simplified
PCI layout, same as isotp_bringup_probe.py / uds_seam4_probe.py.

Run:
    python uds_seam5_probe.py                 # uses ./config.json
    python uds_seam5_probe.py --config other.json
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
UDS_SID_SECURITY_ACCESS = 0x27
UDS_SID_NEGATIVE_RESPONSE = 0x7F
UDS_NRC_REQUEST_SEQUENCE_ERROR = 0x24
UDS_NRC_INVALID_KEY = 0x35

# shared/diag/include/uds_security_access.h -- not a secret, see the honesty note there.
KEY_XOR_CONST = 0xA5A5A5A5


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


def request_seed(bus: can.BusABC, cfg: dict) -> bytes:
    """Returns the raw response (expected [0x27, 0x01, seed(4, BE)])."""
    return uds_request(bus, cfg, bytes([UDS_SID_SECURITY_ACCESS, 0x01]))


def send_key(bus: can.BusABC, cfg: dict, key: int) -> bytes:
    return uds_request(bus, cfg, bytes([UDS_SID_SECURITY_ACCESS, 0x02]) + key.to_bytes(4, "big"))


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
        # 1. requestSeed -> positive, 4-byte seed
        resp = request_seed(bus, cfg)
        ok = len(resp) == 6 and resp[0] == UDS_SID_SECURITY_ACCESS and resp[1] == 0x01
        seed = int.from_bytes(resp[2:6], "big") if ok else 0
        checks.append(("requestSeed -> positive, 4-byte seed", ok, resp))

        # 2. sendKey with the correct key -> positive
        resp = send_key(bus, cfg, seed ^ KEY_XOR_CONST)
        ok = resp == bytes([UDS_SID_SECURITY_ACCESS, 0x02])
        checks.append(("sendKey (correct) -> positive, unlocked", ok, resp))

        # 3. sendKey again immediately (no fresh seed) -> sequence error
        resp = uds_request(bus, cfg, bytes([UDS_SID_SECURITY_ACCESS, 0x02]) + (0).to_bytes(4, "big"))
        expected = bytes([UDS_SID_NEGATIVE_RESPONSE, UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUEST_SEQUENCE_ERROR])
        ok = resp == expected
        checks.append(("sendKey without a fresh seed -> sequence error", ok, resp))

        # 4. requestSeed again (fresh seed, should differ from the first)
        resp2 = request_seed(bus, cfg)
        ok = len(resp2) == 6 and resp2[0] == UDS_SID_SECURITY_ACCESS and resp2[1] == 0x01
        seed2 = int.from_bytes(resp2[2:6], "big") if ok else 0
        ok = ok and (seed2 != seed)
        checks.append(("requestSeed again -> a fresh (different) seed", ok, resp2))

        # 5. sendKey with a WRONG key -> invalid key, relocks
        resp = send_key(bus, cfg, (seed2 ^ KEY_XOR_CONST) ^ 0x1)  # deliberately wrong
        expected = bytes([UDS_SID_NEGATIVE_RESPONSE, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY])
        ok = resp == expected
        checks.append(("sendKey (wrong) -> invalid key", ok, resp))

        # 6. Retrying with the (would-have-been) correct key for seed2 -> sequence
        #    error, since the wrong attempt above relocked (needs a fresh seed).
        resp = send_key(bus, cfg, seed2 ^ KEY_XOR_CONST)
        expected = bytes([UDS_SID_NEGATIVE_RESPONSE, UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUEST_SEQUENCE_ERROR])
        ok = resp == expected
        checks.append(("sendKey after a wrong attempt -> sequence error (relocked)", ok, resp))
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
    return 0 if passed == len(checks) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify the FBL's real SecurityAccess round trip (M3 Seam 5).")
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
