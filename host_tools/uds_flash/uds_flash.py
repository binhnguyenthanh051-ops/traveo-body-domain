#!/usr/bin/env python3
"""uds_flash.py — reflash the Node A FBL over CAN (M3 Seam 7, the full download).

Drives the complete UDS programming sequence against the FBL's diagnostic
stack: enter programming session, unlock security, erase, download the
(stamped) app image via requestDownload / transferData / requestTransferExit,
CRC-verify it with routineControl, and ECUReset -- after which the FBL's own
boot decision re-verifies the image and jumps to it.

Wire protocol is this project's simplified PCI / UDS (shared/diag) -- NOT
standard ISO 15765-2/14229, so a generic UDS tester won't interoperate. The
seed/key transform and the routine/download request layouts are the
project's own (see shared/diag/include/*).

The image handed in is ALREADY stamped by the build (M4: SHA-256 header +
hash/signature/key_id trailer, via host_tools/sign_image.py). This tool
downloads it verbatim -- it does NOT re-stamp -- and only pads up to the
512-byte flash program-row so every transferData block is row-aligned.

Usage:
    python uds_flash.py path/to/gateway_app.hex
    python uds_flash.py app.hex --config other.json
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import can  # python-can >= 4.0

DEFAULT_CONFIG = Path(__file__).with_name("config.json")

# ---- ISO-TP simplified PCI (shared/diag/include/isotp_types.h) ----
PCI_SF, PCI_FF, PCI_CF, PCI_FC = 0x00, 0x10, 0x20, 0x30
FC_CTS = 0x00
FF_INITIAL_LEN = 59

# ---- UDS SIDs / subfunctions (shared/diag/include/uds_types.h) ----
SID_SESSION_CONTROL = 0x10
SID_ECU_RESET = 0x11
SID_SECURITY_ACCESS = 0x27
SID_ROUTINE_CONTROL = 0x31
SID_REQUEST_DOWNLOAD = 0x34
SID_TRANSFER_DATA = 0x36
SID_REQUEST_TRANSFER_EXIT = 0x37
SID_NEGATIVE = 0x7F

SESSION_PROGRAMMING = 0x02
SECURITY_KEY_XOR = 0xA5A5A5A5           # uds_security_access.h
ROUTINE_ERASE = 0xFF00                   # uds_routine_control.h
ROUTINE_CHECK_IMAGE = 0xFF01
FLASH_ROW = 512                          # CY_FLASH_SIZEOF_ROW


def parse_int(v: object) -> int:
    return int(v, 0) if isinstance(v, str) else int(v)


# --------------------------------------------------------------------------
# Intel HEX -> a flat {address: byte} image, then extract the app region.
# --------------------------------------------------------------------------
def parse_intel_hex(path: Path) -> dict[int, int]:
    mem: dict[int, int] = {}
    upper = 0
    with open(path, "r", encoding="ascii") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line[0] != ":":
                continue
            b = bytes.fromhex(line[1:])
            count, addr_hi, addr_lo, rectype = b[0], b[1], b[2], b[3]
            data = b[4:4 + count]
            if (sum(b) & 0xFF) != 0:
                raise ValueError(f"{path}:{lineno}: bad checksum")
            if rectype == 0x00:            # data
                base = (upper << 16) + (addr_hi << 8) + addr_lo
                for i, byte in enumerate(data):
                    mem[base + i] = byte
            elif rectype == 0x04:          # extended linear address (upper 16 bits)
                upper = (data[0] << 8) + data[1]
            elif rectype == 0x02:          # extended segment address
                upper = ((data[0] << 8) + data[1]) >> 12
            elif rectype == 0x01:          # EOF
                break
            # types 03/05 (start address) ignored
    return mem


def extract_app_body(mem: dict[int, int], app_base: int) -> bytes:
    """Contiguous bytes from app_base to the highest byte at/above it.
    Gaps (unwritten addresses) are filled with 0xFF (erased flash)."""
    app_addrs = [a for a in mem if a >= app_base]
    if not app_addrs:
        raise ValueError(f"no data at or above app_base {app_base:#x} in the hex")
    top = max(app_addrs)
    return bytes(mem.get(app_base + i, 0xFF) for i in range(top - app_base + 1))


def build_download_image(hex_path: Path, app_base: int) -> bytes:
    """The app hex is ALREADY stamped by the build (M4: SHA-256 header at
    FBL_APP_HEADER_OFFSET + hash/signature/key_id trailer, from sign_image.py).
    Download it verbatim -- re-stamping here would clobber that header
    (image_len -> full length) and overwrite the signature trailer. Pad only,
    up to a whole flash program row so every transferData block is row-aligned."""
    stamped = extract_app_body(parse_intel_hex(hex_path), app_base)
    pad = (-len(stamped)) % FLASH_ROW
    return stamped + b"\xff" * pad


# --------------------------------------------------------------------------
# ISO-TP + UDS request/response over CAN
# --------------------------------------------------------------------------
class UdsClient:
    def __init__(self, bus: can.BusABC, cfg: dict):
        self.bus = bus
        self.bus_cfg = cfg["bus"]
        self.req_id = parse_int(cfg["uds"]["request_id"])
        self.resp_id = parse_int(cfg["uds"]["response_id"])
        self.timeout = float(cfg["uds"].get("timeout_s", 3.0))

    def _send_frame(self, data: bytes) -> None:
        self.bus.send(can.Message(
            arbitration_id=self.req_id, is_extended_id=False,
            is_fd=self.bus_cfg.get("can_fd", True),
            bitrate_switch=self.bus_cfg.get("bitrate_switch", True),
            data=data,
        ))

    def _recv_frame(self, deadline: float) -> can.Message:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no frame on 0x{self.resp_id:03X}")
            msg = self.bus.recv(timeout=remaining)
            if msg is not None and msg.arbitration_id == self.resp_id:
                return msg

    def _isotp_send(self, payload: bytes) -> None:
        if len(payload) <= 62:
            self._send_frame(bytes([PCI_SF, len(payload)]) + payload)
            return
        # First frame + (wait FC) + consecutive frames.
        self._send_frame(bytes([PCI_FF]) + len(payload).to_bytes(4, "big")
                         + payload[:FF_INITIAL_LEN])
        fc = self._recv_frame(time.monotonic() + self.timeout)
        if (fc.data[0] & 0xF0) != PCI_FC or (fc.data[0] & 0x0F) != FC_CTS:
            raise RuntimeError(f"expected flow-control CTS, got {fc.data[0]:#04x}")
        rest, seq = payload[FF_INITIAL_LEN:], 1
        while rest:
            self._send_frame(bytes([PCI_CF | (seq & 0x0F)]) + rest[:63])
            rest, seq = rest[63:], seq + 1

    def _isotp_recv(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        first = self._recv_frame(deadline)
        t = first.data[0] & 0xF0
        if t == PCI_SF:
            return bytes(first.data[2:2 + first.data[1]])
        if t != PCI_FF:
            raise RuntimeError(f"expected SF/FF, got PCI {first.data[0]:#04x}")
        total = int.from_bytes(first.data[1:5], "big")
        buf = bytearray(first.data[5:])
        self._send_frame(bytes([PCI_FC | FC_CTS, 8, 0]))
        exp = 1
        while len(buf) < total:
            cf = self._recv_frame(deadline)
            if (cf.data[0] & 0xF0) != PCI_CF or (cf.data[0] & 0x0F) != (exp & 0x0F):
                raise RuntimeError("consecutive-frame sequence error")
            buf.extend(cf.data[1:1 + (total - len(buf))])
            exp += 1
        return bytes(buf[:total])

    def request(self, payload: bytes, expect_response: bool = True) -> bytes:
        self._isotp_send(payload)
        if not expect_response:
            return b""
        resp = self._isotp_recv()
        if resp and resp[0] == SID_NEGATIVE:
            sid = resp[1] if len(resp) > 1 else 0
            nrc = resp[2] if len(resp) > 2 else 0
            raise RuntimeError(f"negative response to SID {sid:#04x}: NRC {nrc:#04x}")
        # This stack echoes the request SID on a positive response (not the
        # standard UDS +0x40 convention).
        if resp and resp[0] != payload[0]:
            raise RuntimeError(f"unexpected response SID {resp[0]:#04x}")
        return resp


# --------------------------------------------------------------------------
# The programming sequence
# --------------------------------------------------------------------------
def flash(client: UdsClient, image: bytes, app_base: int, large_sector: int) -> None:
    print(f"  image: {len(image)} bytes ({len(image) // FLASH_ROW} rows) to 0x{app_base:08X}")

    print("  [1] enter programming session (0x10 02)")
    client.request(bytes([SID_SESSION_CONTROL, SESSION_PROGRAMMING]))

    print("  [2] security access (0x27 seed/key)")
    seed_resp = client.request(bytes([SID_SECURITY_ACCESS, 0x01]))
    seed = int.from_bytes(seed_resp[2:6], "big")
    key = seed ^ SECURITY_KEY_XOR
    client.request(bytes([SID_SECURITY_ACCESS, 0x02]) + key.to_bytes(4, "big"))

    erase_len = (len(image) + large_sector - 1) // large_sector * large_sector
    print(f"  [3] erase 0x{erase_len:X} bytes (0x31 {ROUTINE_ERASE:04X})")
    client.request(bytes([SID_ROUTINE_CONTROL, 0x01])
                   + ROUTINE_ERASE.to_bytes(2, "big")
                   + app_base.to_bytes(4, "big") + erase_len.to_bytes(4, "big"))

    print(f"  [4] requestDownload (0x34) addr=0x{app_base:08X} size=0x{len(image):X}")
    rd = client.request(bytes([SID_REQUEST_DOWNLOAD])
                        + app_base.to_bytes(4, "big") + len(image).to_bytes(4, "big"))
    max_block = int.from_bytes(rd[1:3], "big") if len(rd) >= 3 else FLASH_ROW
    block = max(FLASH_ROW, (max_block // FLASH_ROW) * FLASH_ROW)
    print(f"      server maxNumberOfBlockLength=0x{max_block:X}; using {block}-byte blocks")

    print("  [5] transferData (0x36) ...")
    seq, off, t0 = 1, 0, time.monotonic()
    while off < len(image):
        chunk = image[off:off + block]
        client.request(bytes([SID_TRANSFER_DATA, seq & 0xFF]) + chunk)
        off += len(chunk)
        seq += 1
        if seq % 8 == 0:
            print(f"      {off}/{len(image)} bytes")
    print(f"      {off}/{len(image)} bytes in {time.monotonic() - t0:.1f}s")

    print("  [6] requestTransferExit (0x37)")
    client.request(bytes([SID_REQUEST_TRANSFER_EXIT]))

    print(f"  [7] verify image (0x31 {ROUTINE_CHECK_IMAGE:04X})")
    # Positive response: [SID][routineType][routineId_hi][routineId_lo][status]
    # -- the status byte is at index 4 (index 3 is the routineId low byte). A
    # failed check comes back as a negative response (raised in request()), not
    # a positive with a non-zero status, but we still assert status == 0.
    chk = client.request(bytes([SID_ROUTINE_CONTROL, 0x01])
                         + ROUTINE_CHECK_IMAGE.to_bytes(2, "big"))
    status = chk[4] if len(chk) >= 5 else 0xFF
    if status != 0x00:
        raise RuntimeError(f"image check failed (status 0x{status:02X})")
    print("      image verify OK (SHA-256 + signature)")

    print("  [8] ECUReset (0x11 01) -- FBL resets, re-verifies, jumps to the app")
    client.request(bytes([SID_ECU_RESET, 0x01]), expect_response=False)
    # ECUReset has no response (the FBL resets instead), so nothing keeps the
    # bus alive here as every earlier step did. python-can's send() only QUEUES
    # the frame; without this pause the finally-block bus.shutdown() closes the
    # channel before the driver transmits it, the FBL never sees the request,
    # and it stays resident (silicon-verified during Seam 7: the reset event
    # was never hit). Give the frame time to go out and the FBL time to reset.
    time.sleep(0.5)


def build_bus(cfg: dict) -> can.BusABC:
    itf, bus = cfg["interface"], cfg["bus"]
    kwargs = dict(
        interface=itf["backend"], channel=itf["channel"],
        app_name=itf.get("app_name", "python-can"),
        fd=bus.get("can_fd", True), bitrate=bus["nominal_bitrate"],
        data_bitrate=bus["data_bitrate"], receive_own_messages=False,
    )
    if itf.get("serial") is not None:
        kwargs["serial"] = itf["serial"]
    return can.Bus(**kwargs)


def main() -> int:
    ap = argparse.ArgumentParser(description="Reflash the Node A FBL over CAN (M3 Seam 7).")
    ap.add_argument("hex", type=Path, help="app image (Intel HEX), e.g. gateway_app.hex")
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    app_base = parse_int(cfg["image"]["app_base"])
    large_sector = parse_int(cfg["image"].get("large_sector_size", 0x8000))

    print(f"building download image from {args.hex} ...")
    image = build_download_image(args.hex, app_base)

    try:
        bus = build_bus(cfg)
    except can.CanError as exc:
        print(f"error: could not open CAN interface: {exc}", file=sys.stderr)
        return 2

    try:
        client = UdsClient(bus, cfg)
        flash(client, image, app_base, large_sector)
    except (TimeoutError, RuntimeError) as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        bus.shutdown()

    print("\nOK: download complete, ECUReset sent. Confirm the app is running")
    print("(LED behaviour) — the FBL's boot decision verifies and jumps to it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
