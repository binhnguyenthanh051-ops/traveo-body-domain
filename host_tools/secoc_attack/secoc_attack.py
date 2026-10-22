#!/usr/bin/env python3
"""secoc_attack.py -- M5 Stage 6: inject forged / tampered / replayed SecOC frames.

The attacker model is a node on the bus WITHOUT the key: it can send anything
and record anything, but cannot compute a MAC. Each case must be refused by
Node B with exactly one REJECT event and the actuator holding its state.

    python host_tools/secoc_attack/secoc_attack.py all
    python host_tools/secoc_attack/secoc_attack.py replay
    python host_tools/secoc_attack/secoc_attack.py replay --save old_0x121.json
    python host_tools/secoc_attack/secoc_attack.py replay --load old_0x121.json

Cases (M5 bring-up plan Stage 6):
  forged      0x120 "unlock" (01), no trailer     -> REJECT_MAC reason 2 (bad length)
  forged-mac  0x120 "unlock" + random 12 B trailer -> REJECT_MAC reason 1 (bad MAC)
  bitflip     genuine 0x121, one MAC bit flipped   -> REJECT_MAC reason 1
  replay      genuine 0x121, resent ~1 s later     -> REJECT_FRESHNESS (stale counter)
  crossid     genuine 0x121 bytes sent on 0x120    -> REJECT_MAC reason 1 (Data ID bound)

Replay across a reset (M5B, G2): `replay --save FILE` captures a genuine 0x121 and
writes it to FILE without sending it. Reset Node B, wait for the resync, then
`replay --load FILE` sends that frame -> REJECT_FRESHNESS (epoch below the floor).

Node A only ever sends 0x121 (courtesy light), so the captured genuine frame is a
0x121; the forgeries target 0x120, the door. Reuses can_echo_probe's config.json.
"""
import argparse
import datetime
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


def epoch_of(genuine: bytes, pdu_len: int = LIGHT_PDU_LEN) -> int:
    """Freshness epoch (u16 LE) of a secured frame."""
    return int.from_bytes(genuine[pdu_len:pdu_len + 2], "little")


def frame_record(arb_id: int, data: bytes, captured_at: str) -> dict:
    """A saved frame, readable in the bench folder. epoch/counter are for the
    reader; only id + data are sent back."""
    return {"id": f"0x{arb_id:03X}", "data": data.hex(),
            "epoch": epoch_of(data), "counter": counter_of(data),
            "captured_at": captured_at}


def frame_from_record(rec: dict) -> tuple[int, bytes]:
    """(id, data) from a saved record; refuses anything that isn't a secured 0x121."""
    arb_id = int(rec["id"], 16)
    data = bytes.fromhex(rec["data"])
    if arb_id != ID_LIGHT or len(data) < LIGHT_PDU_LEN + TRAILER_LEN:
        raise ValueError(f"not a secured 0x121 frame: id {rec['id']}, {len(data)} B")
    return arb_id, data


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


def save_genuine(bus: can.BusABC, path: pathlib.Path) -> bool:
    """replay --save: capture one genuine 0x121 and write it to path. Sends nothing."""
    genuine = capture(bus, ID_LIGHT)
    if genuine is None:
        print("  no genuine 0x121 seen in 2 s -- is Node A on the bus and running?")
        return False
    stamp = datetime.datetime.now().isoformat(timespec="seconds")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(frame_record(ID_LIGHT, genuine, stamp), indent=2) + "\n",
                    encoding="utf-8")
    print(f"  saved genuine 0x121 (epoch {epoch_of(genuine)} ctr {counter_of(genuine)})"
          f" -> {path}")
    return True


def replay_saved(bus: can.BusABC, path: pathlib.Path) -> bool:
    """replay --load: send a frame saved earlier, e.g. before a Node B reset."""
    arb_id, data = frame_from_record(json.loads(path.read_text(encoding="utf-8")))
    send(bus, arb_id, data,
         f"expect REJECT_FRESHNESS 0x121 (saved epoch {epoch_of(data)} ctr {counter_of(data)})")
    return True


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
    keep = ap.add_mutually_exclusive_group()
    keep.add_argument("--save", type=pathlib.Path, metavar="FILE",
                      help="replay only: capture a genuine 0x121 to FILE, send nothing")
    keep.add_argument("--load", type=pathlib.Path, metavar="FILE",
                      help="replay only: send the frame saved in FILE")
    args = ap.parse_args()
    if (args.save or args.load) and args.case != "replay":
        ap.error("--save/--load only go with the 'replay' case")
    if args.load is not None:
        try:
            frame_from_record(json.loads(args.load.read_text(encoding="utf-8")))
        except (OSError, ValueError, KeyError) as exc:
            ap.error(f"--load {args.load}: {exc}")

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    try:
        bus = open_bus(cfg)
    except Exception as exc:  # the XL driver raises several types
        print(f"error: could not open the CAN interface: {exc}", file=sys.stderr)
        return 1
    ok = True
    try:
        if args.save is not None:
            print("\n[replay --save]")
            ok = save_genuine(bus, args.save)
        elif args.load is not None:
            print("\n[replay --load]")
            ok = replay_saved(bus, args.load)
        else:
            for case in (CASES if args.case == "all" else (args.case,)):
                ok = run_case(bus, case) and ok
                time.sleep(0.5)   # keep each case's log event apart
    finally:
        bus.shutdown()
    if args.save is not None:
        print("\nNow reset Node B, wait for the resync, then run: replay --load", args.save)
        return 0 if ok else 2
    print("\nNow check Node B's log: one REJECT per case, no ACCEPT on 0x120, door still locked.")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
