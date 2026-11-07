#!/usr/bin/env python3
"""Append an embedded payload to the Comet loader executable (dist/Comet.exe).

Payload layout:
    [u32 name_len][name utf8][u64 data_len][data] ...
Footer (last 16 bytes):
    b"BSLDPAY1" + u64 payload_size

The operation is idempotent: an existing footer payload is stripped first.
"""

import argparse
import os
import struct
import sys

MAGIC = b"BSLDPAY1"


def strip_payload(data: bytes) -> bytes:
    if len(data) >= 16 and data[-16:-8] == MAGIC:
        size = struct.unpack("<Q", data[-8:])[0]
        if size <= len(data) - 16:
            return data[: len(data) - 16 - size]
    return data


def build_payload(entries):
    out = bytearray()
    for name, path in entries:
        if not os.path.isfile(path):
            print(f"[pack] skip missing: {path}")
            continue
        with open(path, "rb") as handle:
            blob = handle.read()
        raw_name = name.encode("utf-8")
        out += struct.pack("<I", len(raw_name))
        out += raw_name
        out += struct.pack("<Q", len(blob))
        out += blob
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--file", action="append", default=[], metavar="NAME=PATH")
    args = parser.parse_args()

    if not os.path.isfile(args.exe):
        print(f"[pack] loader not found: {args.exe}")
        return 1

    entries = []
    for item in args.file:
        if "=" not in item:
            print(f"[pack] bad --file value: {item}")
            return 1
        name, path = item.split("=", 1)
        entries.append((name, path))

    with open(args.exe, "rb") as handle:
        data = handle.read()

    base = strip_payload(data)
    payload = build_payload(entries)
    if not payload:
        print("[pack] nothing to embed")
        return 1

    result = base + payload + MAGIC + struct.pack("<Q", len(payload))
    with open(args.exe, "wb") as handle:
        handle.write(result)

    print(f"[pack] embedded {len(entries)} files, payload={len(payload)} bytes -> {args.exe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
