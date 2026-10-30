#!/usr/bin/env python3
"""check_key_residency.py -- REQ-SECOC-011 evidence from the built images.

REQ-SECOC-011: the AES-CMAC secret lives ONLY in each node's CM0+ image; the
application core holds no key bytes. Where the key sits is linker composition,
so it is `@design-only` in the code. This script turns it into evidence: it
reads the key from security/include/secoc_shared_secret.h (the single source,
so the bytes are never copied here) and searches each ELF for them.

    CM0+ images   must CONTAIN the key  (the positive control: if the search
                                         can't find it there, the method is wrong)
    app images    must NOT contain it   (CM4 FBL, CM4 app, CM7 app)

The key is a published portfolio dummy (ADR-0021 D6). The script prints where
it found it, never the bytes.

Exit 0 when every image matches its role, 1 otherwise, 2 on a missing input.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
KEY_HEADER = ROOT / "security" / "include" / "secoc_shared_secret.h"

# (role, path relative to the repo). The Debug build of each MTB project.
IMAGES: list[tuple[str, str]] = [
    ("cm0p", "node_a_gateway/bootloader/proj_cm0p/build/CYTVII-B-E-1M-SK/Debug/gateway_fbl_cm0p.elf"),
    ("app",  "node_a_gateway/bootloader/proj_cm4/build/CYTVII-B-E-1M-SK/Debug/gateway_fbl.elf"),
    ("app",  "node_a_gateway/app/build/CYTVII-B-E-1M-SK/Debug/gateway_app.elf"),
    ("cm0p", "node_b_actuator/proj_cm0p/build/APP_KIT_T2G-B-H_LITE/Debug/actuator_cm0p.elf"),
    ("app",  "node_b_actuator/proj_cm7/build/APP_KIT_T2G-B-H_LITE/Debug/actuator_cm7.elf"),
]

_ARRAY_RE = re.compile(r"secoc_shared_secret\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", re.S)
_BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{1,2})[uU]?")


def read_key(header: pathlib.Path) -> bytes:
    """The key bytes from the header's `secoc_shared_secret[...] = { ... }`."""
    m = _ARRAY_RE.search(header.read_text(encoding="utf-8"))
    if not m:
        raise ValueError(f"no secoc_shared_secret initializer in {header}")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    key = bytes(int(b, 16) for b in _BYTE_RE.findall(body))
    if len(key) != 16:
        raise ValueError(f"expected a 16-byte AES-128 key, parsed {len(key)} bytes")
    return key


def check(images: list[tuple[str, pathlib.Path]], key: bytes) -> list[str]:
    """One line per image; a line starting with FAIL means the role is violated."""
    lines = []
    for role, path in images:
        data = path.read_bytes()
        found = data.find(key)
        has_key = found >= 0
        ok = has_key if role == "cm0p" else not has_key
        where = f"key at file offset 0x{found:X}" if has_key else "key absent"
        lines.append(f"{'ok  ' if ok else 'FAIL'} {role:4} {where:28} {path.relative_to(ROOT).as_posix()}")
    return lines


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.parse_args(argv)
    key = read_key(KEY_HEADER)
    images = [(role, ROOT / rel) for role, rel in IMAGES]
    missing = [p for _, p in images if not p.is_file()]
    if missing:
        for p in missing:
            print(f"missing: {p.relative_to(ROOT).as_posix()} (build the MTB project first)")
        return 2
    lines = check(images, key)
    print("\n".join(lines))
    return 1 if any(ln.startswith("FAIL") for ln in lines) else 0


if __name__ == "__main__":
    sys.exit(main())
