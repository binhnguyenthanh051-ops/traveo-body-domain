#!/usr/bin/env python3
"""fbl_image_stamp.py — stamp an application binary so the FBL accepts it.

Writes the descriptive image header at FBL_APP_HEADER_OFFSET and appends a CRC32
integrity trailer, exactly matching fbl_app_image_valid (ADR-0008 D3):

    [ app_base .. app_base+image_len )   <- covered by the digest
        app_base + header_offset : { magic, hdr_version, hdr_size, image_len }
    [ app_base + image_len .. +4 )       <- CRC32 trailer (EXCLUDED from the digest)

The app's linker MUST reserve the header slot at `header_offset` (a few bytes,
past the vector table) so stamping does not clobber code. `header_offset` MUST
equal the FBL's FBL_APP_HEADER_OFFSET.

The FBL's M1 digest is standard CRC-32 (poly 0xEDB88320), which is exactly
Python's zlib.crc32 — so a stamped image is byte-for-byte what boot_crc32 expects.

Usage:
    python host_tools/fbl_image_stamp.py app.bin app_stamped.bin --header-offset 0x100
"""
from __future__ import annotations

import argparse
import struct
import zlib

# --- keep in sync with shared/boot/include/boot_types.h ---
FBL_APP_HEADER_MAGIC = 0xA9900D01
FBL_HDR_VERSION = 1
FBL_HDR_SIZE = 12            # u32 magic + u16 hdr_version + u16 hdr_size + u32 image_len
FBL_DIGEST_SIZE = 4          # CRC32 (M1); M4 swaps to SHA-256 + signature
DEFAULT_HEADER_OFFSET = 0x100  # FBL_APP_HEADER_OFFSET; clears the CM4 vector table (0x80)


def stamp_image(body: bytes, header_offset: int = DEFAULT_HEADER_OFFSET) -> bytes:
    """Return `body` with the header written at `header_offset` and a CRC32
    trailer appended. `body` is the raw app image starting at the app base."""
    buf = bytearray(body)
    image_len = len(buf)
    if header_offset + FBL_HDR_SIZE > image_len:
        raise ValueError(
            f"image ({image_len} B) too small for a {FBL_HDR_SIZE}-byte header "
            f"at offset {header_offset:#x}"
        )
    struct.pack_into(
        "<IHHI", buf, header_offset,
        FBL_APP_HEADER_MAGIC, FBL_HDR_VERSION, FBL_HDR_SIZE, image_len,
    )
    crc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF
    return bytes(buf) + struct.pack("<I", crc)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", help="raw app binary (objcopy -O binary), from the app base")
    ap.add_argument("output", help="stamped binary to flash at the app base")
    ap.add_argument(
        "--header-offset", type=lambda x: int(x, 0), default=DEFAULT_HEADER_OFFSET,
        help=f"must equal FBL_APP_HEADER_OFFSET (default {DEFAULT_HEADER_OFFSET:#x})",
    )
    args = ap.parse_args(argv)

    with open(args.input, "rb") as f:
        body = f.read()
    out = stamp_image(body, args.header_offset)
    with open(args.output, "wb") as f:
        f.write(out)

    crc = struct.unpack("<I", out[-FBL_DIGEST_SIZE:])[0]
    print(
        f"stamped {args.input} -> {args.output}: "
        f"image_len={len(body)} (0x{len(body):x}) B, header@0x{args.header_offset:x}, "
        f"crc32=0x{crc:08x}, total={len(out)} B"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
