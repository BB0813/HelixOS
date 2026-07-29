#!/usr/bin/env python3
"""Embed a binary file as a C byte array header."""
from __future__ import annotations
import argparse
import pathlib
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-n", "--name", required=True, help="C symbol base name")
    args = ap.parse_args()

    data = pathlib.Path(args.input).read_bytes()
    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        f"/* Auto-generated from {args.input} — do not edit */",
        "#pragma once",
        f"static const unsigned char {args.name}[] = {{",
    ]
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append(f"static const unsigned int {args.name}_len = {len(data)};")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out} ({len(data)} bytes as {args.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
