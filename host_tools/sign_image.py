#!/usr/bin/env python3
"""sign_image.py — sign an application binary so the M4 FBL authenticates it.

STUB (M4 step 5 implements). The layered design is agreed (ADR-0016/0017/0019)
and the host tests are written; sign_image() raises NotImplementedError until
the implementation lands, so host_tools/tests/test_sign_image.py fails by
design until then.

This is the authenticity sibling of fbl_image_stamp.py. It reuses the SAME
descriptive header (ADR-0008 D3) and the SAME covered range, and only swaps the
integrity trailer from a CRC32 (M1/M3) to the M4 trailer:

    [ app_base .. app_base+image_len )        <- COVERED by the hash
        app_base + header_offset : { magic, hdr_version, hdr_size, image_len }
    [ app_base + image_len .. )               <- EXCLUDED trailer (ADR-0019):
        hash[32]        SHA-256 over the covered range
        signature[64]   ECDSA P-256 (raw r||s) over the hash
        key_id (u32 LE) which public key verifies this image (ADR-0019 D3)

The private key lives ONLY on the build host (ADR-0019 D1) — never on the
device. The device holds only the matching public key, compiled into the M0+
image (ADR-0019 D2). The FBL asks the M0+ to verify (ADR-0016 D2); this tool is
the other end of that trust: it is the only place a valid signature is produced.

Usage (once implemented):
    python host_tools/sign_image.py app.bin app_signed.bin \\
        --key ec_p256_private.pem --key-id 1 --header-offset 0x100
"""
from __future__ import annotations

import argparse
import hashlib
import struct

# --- keep in sync with shared/boot/include/boot_types.h and ADR-0008 D3 ---
FBL_APP_HEADER_MAGIC = 0xA9900D01
FBL_HDR_VERSION = 1
FBL_HDR_SIZE = 12                 # u32 magic + u16 hdr_version + u16 hdr_size + u32 image_len
DEFAULT_HEADER_OFFSET = 0x100     # == FBL_APP_HEADER_OFFSET (clears the CM4 vector table)

# --- M4 trailer geometry (ADR-0019 D4) ---
HASH_SIZE = 32                    # SHA-256
SIG_SIZE = 64                     # ECDSA P-256, raw r||s (32 + 32)
KEY_ID_SIZE = 4                   # u32 LE
TRAILER_SIZE = HASH_SIZE + SIG_SIZE + KEY_ID_SIZE


def sign_image(body: bytes, private_key, key_id: int,
               header_offset: int = DEFAULT_HEADER_OFFSET) -> bytes:
    """Return `body` with the header written at `header_offset` and the M4
    hash+signature+key_id trailer appended (ADR-0019 D4). `private_key` is a
    cryptography EC private key on the P-256 curve.

    The signature is over the 32-byte hash (ADR-0016 D2), signed with
    ECDSA(SHA-256) — i.e. the curve op prehashes the digest — and stored as raw
    r||s (32+32, big-endian). The FBL verifies with the same scheme."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import (
        Prehashed,
        decode_dss_signature,
    )

    buf = bytearray(body)
    image_len = len(buf)
    if header_offset + FBL_HDR_SIZE > image_len:
        raise ValueError(
            f"image ({image_len} B) too small for a {FBL_HDR_SIZE}-byte header "
            f"at offset {header_offset:#x}"
        )

    # Descriptive header at the fixed offset (identical to fbl_image_stamp).
    struct.pack_into(
        "<IHHI", buf, header_offset,
        FBL_APP_HEADER_MAGIC, FBL_HDR_VERSION, FBL_HDR_SIZE, image_len,
    )

    # Stage 1: hash over the covered range [0, image_len).
    digest = hashlib.sha256(bytes(buf)).digest()

    # Stage 2: ECDSA P-256 signature over the digest DIRECTLY (Prehashed), i.e.
    # sign the 32-byte SHA-256 as-is — NOT sign(digest, ECDSA(SHA256())), which
    # would hash it a second time. The target's Cy_Crypto_Core_ECC_VerifyHash
    # takes the pre-computed digest and does not re-hash, so the signature must
    # be over `digest` itself or verification fails (M4 Seam-3 bench finding).
    der_sig = private_key.sign(digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    r, s = decode_dss_signature(der_sig)
    raw_sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")

    trailer = digest + raw_sig + struct.pack("<I", key_id)
    if len(trailer) != TRAILER_SIZE:  # invariant guard
        raise AssertionError("trailer size mismatch")
    return bytes(buf) + trailer


def parse_trailer(image: bytes, header_offset: int = DEFAULT_HEADER_OFFSET):
    """Split a signed image into (covered_body, hash, signature, key_id) using
    the header's image_len as the covered/excluded boundary (ADR-0008 D3)."""
    if header_offset + FBL_HDR_SIZE + TRAILER_SIZE > len(image):
        raise ValueError("image too small to contain a header + M4 trailer")
    magic, _ver, _size, image_len = struct.unpack_from("<IHHI", image, header_offset)
    if magic != FBL_APP_HEADER_MAGIC:
        raise ValueError(f"bad header magic {magic:#010x}")
    if image_len > len(image) - TRAILER_SIZE:
        raise ValueError("image_len overruns the trailer")

    covered = image[:image_len]
    trailer = image[image_len:image_len + TRAILER_SIZE]
    hash_bytes = trailer[:HASH_SIZE]
    signature = trailer[HASH_SIZE:HASH_SIZE + SIG_SIZE]
    key_id = struct.unpack_from("<I", trailer, HASH_SIZE + SIG_SIZE)[0]
    return covered, hash_bytes, signature, key_id


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", help="raw app binary (objcopy -O binary), from the app base")
    ap.add_argument("output", help="signed binary to flash at the app base")
    ap.add_argument("--key", required=True, help="EC P-256 private key (PEM) — build-host secret")
    ap.add_argument("--key-id", type=lambda x: int(x, 0), default=1,
                    help="key_id written to the trailer (ADR-0019 D3)")
    ap.add_argument("--header-offset", type=lambda x: int(x, 0), default=DEFAULT_HEADER_OFFSET,
                    help=f"must equal FBL_APP_HEADER_OFFSET (default {DEFAULT_HEADER_OFFSET:#x})")
    args = ap.parse_args(argv)

    # Lazy import so the module (and the tests) load without cryptography present.
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    with open(args.key, "rb") as f:
        private_key = load_pem_private_key(f.read(), password=None)
    with open(args.input, "rb") as f:
        body = f.read()

    out = sign_image(body, private_key, args.key_id, args.header_offset)
    with open(args.output, "wb") as f:
        f.write(out)

    print(
        f"signed {args.input} -> {args.output}: "
        f"image_len={len(body)} (0x{len(body):x}) B, header@0x{args.header_offset:x}, "
        f"key_id={args.key_id}, trailer={TRAILER_SIZE} B, total={len(out)} B"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
