"""Tests for sign_image — a signed image must pass the FBL's M4 authenticity
check (ADR-0016 D2), replicated here in Python: SHA-256 over the covered range,
ECDSA P-256 signature over that hash, key_id in the trailer.

Fails against the current stub sign_image.py (NotImplementedError) — M4 step 5
implements it. Mirrors test_fbl_image_stamp.py's "replicate the FBL check in
Python" approach, upgraded from CRC32 to hash+signature."""
import pathlib
import struct
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from sign_image import (  # noqa: E402
    FBL_APP_HEADER_MAGIC,
    HASH_SIZE,
    SIG_SIZE,
    KEY_ID_SIZE,
    TRAILER_SIZE,
    sign_image,
    parse_trailer,
)

# cryptography is a build-host tool dependency (requirements.txt); skip the
# whole module cleanly where it is not installed rather than erroring.
crypto_ec = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.ec")
from cryptography.hazmat.primitives import hashes  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.utils import (  # noqa: E402
    Prehashed,
    encode_dss_signature,
)
from cryptography.exceptions import InvalidSignature  # noqa: E402
import hashlib  # noqa: E402

HEADER_OFFSET = 0x100
KEY_ID = 1


@pytest.fixture
def keypair():
    priv = crypto_ec.generate_private_key(crypto_ec.SECP256R1())
    return priv, priv.public_key()


def fbl_verify(image: bytes, public_key, header_offset: int = HEADER_OFFSET) -> bool:
    """Mirror of the M4 FBL check (ADR-0016 D2): parse header for image_len,
    hash the covered range, verify the trailer signature over that hash with
    the public key. Independent of sign_image's internals — reads raw bytes."""
    if header_offset + 12 + TRAILER_SIZE > len(image):
        return False
    magic, _ver, _size, image_len = struct.unpack_from("<IHHI", image, header_offset)
    if magic != FBL_APP_HEADER_MAGIC:
        return False
    if image_len > len(image) - TRAILER_SIZE:
        return False

    body = image[:image_len]
    trailer = image[image_len:image_len + TRAILER_SIZE]
    stored_hash = trailer[:HASH_SIZE]
    raw_sig = trailer[HASH_SIZE:HASH_SIZE + SIG_SIZE]
    key_id = struct.unpack_from("<I", trailer, HASH_SIZE + SIG_SIZE)[0]

    if key_id != KEY_ID:
        return False
    computed = hashlib.sha256(body).digest()
    if computed != stored_hash:            # stage 1: integrity
        return False

    r = int.from_bytes(raw_sig[:32], "big")
    s = int.from_bytes(raw_sig[32:], "big")
    der = encode_dss_signature(r, s)
    try:                                    # stage 2: authenticity
        # Prehashed: verify the signature against `stored_hash` DIRECTLY, with
        # no second hash — this mirrors the target's Cy_Crypto_Core_ECC_Verify
        # Hash exactly (it takes the pre-computed digest). sign_image must sign
        # the digest the same way (Prehashed), or this fails — the M4 Seam-3
        # double-hash bug this test now guards against.
        public_key.verify(der, stored_hash, crypto_ec.ECDSA(Prehashed(hashes.SHA256())))
        return True
    except InvalidSignature:
        return False


def test_signed_image_passes_fbl_verification(keypair):
    priv, pub = keypair
    body = bytes(range(256)) * 8          # 2048 B, larger than the header offset
    signed = sign_image(body, priv, KEY_ID, HEADER_OFFSET)
    assert fbl_verify(signed, pub, HEADER_OFFSET)


def test_trailer_layout_sizes(keypair):
    priv, _pub = keypair
    body = bytes(2048)
    signed = sign_image(body, priv, KEY_ID, HEADER_OFFSET)
    _magic, _ver, _size, image_len = struct.unpack_from("<IHHI", signed, HEADER_OFFSET)
    assert image_len == len(body)
    assert len(signed) == image_len + TRAILER_SIZE
    assert TRAILER_SIZE == HASH_SIZE + SIG_SIZE + KEY_ID_SIZE


def test_tampered_body_fails_verification(keypair):
    priv, pub = keypair
    signed = bytearray(sign_image(bytes(2048), priv, KEY_ID, HEADER_OFFSET))
    signed[0x500] ^= 0xFF                  # flip a covered byte after the header
    assert not fbl_verify(bytes(signed), pub, HEADER_OFFSET)


def test_wrong_key_fails_verification(keypair):
    priv, _pub = keypair
    other_pub = crypto_ec.generate_private_key(crypto_ec.SECP256R1()).public_key()
    signed = sign_image(bytes(2048), priv, KEY_ID, HEADER_OFFSET)
    assert not fbl_verify(signed, other_pub, HEADER_OFFSET)   # signature won't match a different key


def test_parse_trailer_round_trips(keypair):
    priv, _pub = keypair
    body = bytes(range(256)) * 8
    signed = sign_image(body, priv, KEY_ID, HEADER_OFFSET)
    covered, h, sig, key_id = parse_trailer(signed, HEADER_OFFSET)
    assert covered == signed[:len(body)]
    assert len(h) == HASH_SIZE
    assert len(sig) == SIG_SIZE
    assert key_id == KEY_ID
