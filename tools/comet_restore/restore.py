#!/usr/bin/env python3
"""Reconstruct large source files from base64 chunks and write them in-place."""
from pathlib import Path
import base64, sys

ROOT = Path(__file__).resolve().parents[2]
MAPPING = {
    "loader": ROOT / "src" / "loader.cpp",
    "overlay": ROOT / "src" / "overlay.cpp",
    "runtime": ROOT / "pyexec" / "bs_runtime.py",
}
chunk_dir = Path(__file__).resolve().parent

for key, dest in MAPPING.items():
    parts = sorted(chunk_dir.glob(f"{key}_*.b64"))
    if not parts:
        print(f"missing chunks for {key}", file=sys.stderr)
        sys.exit(1)
    b64 = "".join(p.read_text().strip() for p in parts)
    data = base64.b64decode(b64)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print(f"wrote {dest} ({len(data)} bytes) from {len(parts)} chunks")
print("restore complete")
