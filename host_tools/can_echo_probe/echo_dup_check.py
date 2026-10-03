#!/usr/bin/env python3
"""echo_dup_check.py -- does Node B receive each injected frame once or twice?

Sends N frames on 0x7A0 (Node B echoes non-command IDs) and counts how many echoes
come back for each. One echo per frame = single reception. Two = the injected
frame reached Node B twice (W40 Stage 6: every attack REJECT was logged twice).

    python host_tools/can_echo_probe/echo_dup_check.py
"""
import collections
import json
import pathlib
import sys
import time

import can

HERE = pathlib.Path(__file__).resolve().parent
N = 10
TX_ID = 0x7A0


def main() -> int:
    cfg = json.loads((HERE / "config.json").read_text(encoding="utf-8"))
    itf, b = cfg["interface"], cfg["bus"]
    bus = can.Bus(interface=itf["backend"], channel=itf["channel"],
                  app_name=itf.get("app_name", "python-can"), fd=b.get("can_fd", True),
                  bitrate=b["nominal_bitrate"], data_bitrate=b["data_bitrate"],
                  receive_own_messages=False)
    counts = collections.Counter()
    errors = 0
    try:
        for i in range(N):
            bus.send(can.Message(arbitration_id=TX_ID, is_extended_id=False, is_fd=True,
                                 bitrate_switch=True, data=bytes([0xD0, i])))
            deadline = time.monotonic() + 0.3
            while time.monotonic() < deadline:
                m = bus.recv(timeout=0.05)
                if m is None:
                    continue
                if m.is_error_frame:
                    errors += 1
                elif m.arbitration_id == TX_ID and bytes(m.data[:2]) == bytes([0xD0, i]):
                    counts[i] += 1
    finally:
        bus.shutdown()
    per_frame = [counts[i] for i in range(N)]
    print(f"echoes per frame: {per_frame}   error frames seen: {errors}")
    if all(c == 1 for c in per_frame):
        print("=> single reception: Node B gets each injected frame once.")
    elif all(c >= 2 for c in per_frame):
        print("=> DOUBLE reception: each injected frame reaches Node B twice.")
    else:
        print("=> mixed: some frames doubled, some not.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
