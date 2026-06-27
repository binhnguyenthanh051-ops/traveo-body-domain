"""Tests for fbl_image_stamp — confirm a stamped image passes the FBL's own
validation logic (fbl_app_image_valid, ADR-0008 D3), replicated here in Python."""
import pathlib
import struct
import sys
import zlib

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from fbl_image_stamp import (  # noqa: E402
    FBL_APP_HEADER_MAGIC,
    FBL_DIGEST_SIZE,
    stamp_image,
)

HEADER_OFFSET = 0x100   # == FBL_APP_HEADER_OFFSET (clears the CM4 vector table at 0x80)


def fbl_app_image_valid(image: bytes, header_offset: int = HEADER_OFFSET) -> bool:
    """Mirror of shared/boot/src/boot.c fbl_app_image_valid (digest part)."""
    if header_offset + 12 + FBL_DIGEST_SIZE > len(image):
        return False
    magic, _ver, _size, image_len = struct.unpack_from("<IHHI", image, header_offset)
    if magic != FBL_APP_HEADER_MAGIC:
        return False
    if image_len < header_offset + 12:
        return False
    if image_len > len(image) - FBL_DIGEST_SIZE:
        return False
    body = image[:image_len]
    trailer = image[image_len:image_len + FBL_DIGEST_SIZE]
    digest = (zlib.crc32(body) & 0xFFFFFFFF).to_bytes(FBL_DIGEST_SIZE, "little")
    return digest == trailer


def test_crc_matches_fbl_known_vector():
    # boot_crc32("123456789") == 0xCBF43926; zlib.crc32 must agree.
    assert zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926


def test_stamped_image_passes_fbl_validation():
    body = bytes(range(256)) * 8          # 2048 B, larger than the header offset
    stamped = stamp_image(body, HEADER_OFFSET)
    assert fbl_app_image_valid(stamped, HEADER_OFFSET)


def test_image_len_and_trailer_are_consistent():
    body = bytes(2048)
    stamped = stamp_image(body, HEADER_OFFSET)
    _magic, _ver, _size, image_len = struct.unpack_from("<IHHI", stamped, HEADER_OFFSET)
    assert image_len == len(body)
    assert len(stamped) == image_len + FBL_DIGEST_SIZE


def test_corrupting_the_body_fails_validation():
    stamped = bytearray(stamp_image(bytes(2048), HEADER_OFFSET))
    stamped[0x500] ^= 0xFF                 # flip a covered byte after the header
    assert not fbl_app_image_valid(bytes(stamped), HEADER_OFFSET)


def test_image_too_small_for_header_raises():
    with pytest.raises(ValueError):
        stamp_image(bytes(16), HEADER_OFFSET)
