#!/usr/bin/env python3
"""Pack ChainIt-Setup-<version>.exe from the installer stub + the app payload.

The setup program is a self-contained single file: the stub exe is followed by
the ChainIt-Windows.exe payload and a 52-byte trailer the stub reads back out of
its own image at run time.

    trailer layout (little endian)
      0..7    magic   b"CHAINIT1"
      8..15   uint64  payload size in bytes
      16..47  bytes   SHA-256 of the payload
      48..51  uint32  trailer size (always 52)

    payload offset = filesize - 52 - payload size

Usage:
    python3 installer/pack_installer.py <stub.exe> <ChainIt-Windows.exe> <out.exe> [--version 1.3.0]
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"CHAINIT1"
FOOTER_SIZE = 52


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stub", type=Path, help="chainit-setup-stub.exe")
    ap.add_argument("payload", type=Path, help="ChainIt-Windows.exe to embed")
    ap.add_argument("out", type=Path, help="ChainIt-Setup-<version>.exe")
    ap.add_argument("--version", default="0.0.0")
    args = ap.parse_args()

    stub = args.stub.read_bytes()
    payload = args.payload.read_bytes()

    if not stub[:2] == b"MZ":
        print(f"error: {args.stub} is not a PE executable", file=sys.stderr)
        return 1
    if not payload[:2] == b"MZ":
        print(f"error: {args.payload} is not a PE executable", file=sys.stderr)
        return 1
    if MAGIC in stub[-FOOTER_SIZE:]:
        print("error: the stub already carries a trailer; rebuild it first", file=sys.stderr)
        return 1

    digest = hashlib.sha256(payload).digest()
    footer = MAGIC + struct.pack("<Q", len(payload)) + digest + struct.pack("<I", FOOTER_SIZE)
    assert len(footer) == FOOTER_SIZE, len(footer)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(stub + payload + footer)

    total = args.out.stat().st_size
    print(f"OK  {args.out}")
    print(f"    version       {args.version}")
    print(f"    stub          {len(stub):,} bytes")
    print(f"    payload       {len(payload):,} bytes  sha256 {digest.hex()}")
    print(f"    setup file    {total:,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
