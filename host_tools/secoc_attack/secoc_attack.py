#!/usr/bin/env python3
"""secoc_attack.py -- M5 Stage 6: inject forged / tampered / replayed SecOC frames.

The attacker model is a node on the bus WITHOUT the key: it can send anything
and record anything, but cannot compute a MAC. Each case must be refused by
Node B with exactly one REJECT event and the actuator holding its state.

    python host_tools/secoc_attack/secoc_attack.py all
    python host_tools/secoc_attack/secoc_attack.py replay

Cases (M5 bring-up plan Stage 6):
  forged      0x120 "unlock" (01), no trailer     -> REJECT_MAC reason 2 (bad length)
  forged-mac  0x120 "unlock" + random 12 B trailer -> REJECT_MAC reason 1 (bad MAC)
  bitflip     genuine 0x121, one MAC bit flipped   -> REJECT_MAC reason 1
  replay      genuine 0x121, resent ~1 s later     -> REJECT_FRESHNESS (stale counter)
  crossid     genuine 0x121 bytes sent on 0x120    -> REJECT_MAC reason 1 (Data ID bound)

Node A only ever sends 0x121 (courtesy light), so the captured genuine frame is a
0x121; the forgeries target 0x120, the door. Reuses can_echo_probe's config.json.
"""
import argparse
import json
import os
import pathlib
import sys
import time

import can

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE.parent / "can_echo_probe" / "config.json"

ID_DOOR = 0x120
ID_LIGHT = 0x121
LIGHT_PDU_LEN = 1          # body_msg_pdu_len(0x121)
TRAILER_LEN = 12           # freshness (4) + truncated MAC (8)
DOOR_UNLOCK = 0x01

CASES = ("forged", "forged-mac", "bitflip", "replay", "crossid")


# ---- pure frame crafting (unit-tested, no bus) ------------------------------

def forged_short() -> bytes:
    """A bare unlock command: what an attacker sends if they ignore SecOC."""
    return bytes([DOOR_UNLOCK])


def forged_with_mac(rand: bytes) -> bytes:
    """Unlock + a well-formed but made-up trailer, padded to 16 B like a real frame."""
    assert len(rand) == TRAILER_LEN
    return (bytes([DOOR_UNLOCK]) + rand).ljust(16, b"\x00")


def flip_mac_bit(genuine: bytes, pdu_len: int = LIGHT_PDU_LEN) -> bytes:
    """Flip the lowest bit of the first MAC byte; everything else stays genuine."""
    mac0 = pdu_len + 4
    out = bytearray(genuine)
    out[mac0] ^= 0x01
    return bytes(out)


def counter_of(genuine: bytes, pdu_len: int = LIGHT_PDU_LEN) -> int:
    """Freshness counter (u16 LE) of a secured frame — for reading the replay result."""
    return int.from_bytes(genuine[pdu_len + 2:pdu_len + 4], "little")


# ---- bus side ----------------------------------------------------------------

def open_bus(cfg: dict) -> can.BusABC:
    itf, bus_cfg = cfg["interface"], cfg["bus"]
    kwargs = dict(
        interface=itf["backend"],
        channel=itf["channel"],
        app_name=itf.get("app_name", "python-can"),
        fd=bus_cfg.get("can_fd", True),
        bitrate=bus_cfg["nominal_bitrate"],
        data_bitrate=bus_cfg["data_bitrate"],
        receive_own_messages=False,
    )
    if itf.get("serial") is not None:
        kwargs["serial"] = itf["serial"]
    return can.Bus(**kwargs)


def capture(bus: can.BusABC, arb_id: int, timeout: float = 2.0) -> bytes | None:
    """Record one genuine frame for arb_id off the bus."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        msg = bus.recv(timeout=0.2)
        if msg is not None and not msg.is_error_frame and msg.arbitration_id == arb_id:
            return bytes(msg.data)
    return None


def send(bus: can.BusABC, arb_id: int, data: bytes, label: str) -> None:
    bus.send(can.Message(arbitration_id=arb_id, is_extended_id=False,
                         is_fd=True, bitrate_switch=True, data=data))
    print(f"  sent {arb_id:03X} len={len(data):2d}  {data.hex(' ')}   <- {label}")


def run_case(bus: can.BusABC, case: str) -> bool:
    print(f"\n[{case}]")
    if case == "forged":
        send(bus, ID_DOOR, forged_short(), "expect REJECT_MAC 0x120 reason=2 (bad length)")
        return True
    if case == "forged-mac":
        send(bus, ID_DOOR, forged_with_mac(os.urandom(TRAILER_LEN)),
             "expect REJECT_MAC 0x120 reason=1 (bad MAC)")
        return True

    genuine = capture(bus, ID_LIGHT)
    if genuine is None:
        print("  no genuine 0x121 seen in 2 s -- is Node A on the bus and running?")
        return False
    print(f"  captured genuine 0x121 (ctr {counter_of(genuine)}): {genuine.hex(' ')}")

    if case == "bitflip":
        send(bus, ID_LIGHT, flip_mac_bit(genuine), "expect REJECT_MAC 0x121 reason=1")
    elif case == "replay":
        time.sleep(1.0)   # Node A sends ~50 newer 0x121 meanwhile -> this one is stale
        send(bus, ID_LIGHT, genuine,
             f"expect REJECT_FRESHNESS 0x121 rx_ctr={counter_of(genuine)}")
    elif case == "crossid":
        send(bus, ID_DOOR, genuine, "expect REJECT_MAC 0x120 reason=1 (Data ID bound)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("case", choices=CASES + ("all",))
    ap.add_argument("--config", type=pathlib.Path, default=DEFAULT_CONFIG)
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    try:
        bus = open_bus(cfg)
    except Exception as exc:  # the XL driver raises several types
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        return 1
    ok = True
    try:
        for case in (CASES if args.case == "all" else (args.case,)):
            ok = run_case(bus, case) and ok
            time.sleep(0.5)   # keep each case's log event apart
    finally:
        bus.shutdown()
    print("\nNow check Node B's log: one REJECT per case, no ACCEPT on 0x120, door still locked.")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
