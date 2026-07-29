#!/usr/bin/env python3
"""Add or replace PT_INTERP on an ELF64 ET_EXEC/ET_DYN (append note segment).

For Helix M6 demos we keep a simple approach:
  - Ensure e_type stays ET_EXEC (or set ET_DYN if --dyn)
  - Append a PT_INTERP program header + string in a new LOADable tail if needed

Actually simplest robust approach for our freestanding ELFs:
  Rebuild phdr table in a new buffer with INTERP + original LOADs.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys


def u16(x: int) -> bytes:
    return struct.pack("<H", x & 0xFFFF)


def u32(x: int) -> bytes:
    return struct.pack("<I", x & 0xFFFFFFFF)


def u64(x: int) -> bytes:
    return struct.pack("<Q", x & 0xFFFFFFFFFFFFFFFF)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--interp", default="/lib/ld-helix.so")
    ap.add_argument("--et-dyn", action="store_true", help="set e_type to ET_DYN")
    args = ap.parse_args()

    data = bytearray(open(args.input, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        print("need ELF64", file=sys.stderr)
        return 1

    interp_path = args.interp
    # Guard against MSYS path conversion (/lib → C:/msys64/lib)
    if len(interp_path) >= 2 and interp_path[1] == ":" and ("msys" in interp_path.lower() or "\\" in interp_path or interp_path[0].isalpha()):
        # e.g. C:/msys64/lib/ld-helix.so → /lib/ld-helix.so
        idx = interp_path.lower().find("/lib/")
        if idx >= 0:
            interp_path = interp_path[idx:]
        else:
            interp_path = "/lib/ld-helix.so"
    # Force forward slashes, Unix absolute
    interp_path = interp_path.replace("\\", "/")
    if not interp_path.startswith("/"):
        interp_path = "/" + interp_path

    e_type = struct.unpack_from("<H", data, 16)[0]
    e_entry = struct.unpack_from("<Q", data, 24)[0]
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

    # Collect existing phdrs
    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        ph = data[off : off + e_phentsize]
        p_type = struct.unpack_from("<I", ph, 0)[0]
        if p_type == 3:  # skip old INTERP
            continue
        phdrs.append(bytearray(ph))

    interp = interp_path.encode("ascii") + b"\0"
    # Place interp string and new phdrs after file, page-aligned in file off
    # For our tiny demos, put at end of file; INTERP is not LOAD.
    # Kernel only needs to read path from phdr filesz/offset.

    new_phnum = len(phdrs) + 1
    # Rebuild: keep original file content, append interp string, rewrite phdr table
    # at end for simplicity — also patch e_phoff to new table.

    # Align append
    while len(data) % 8:
        data.append(0)
    interp_off = len(data)
    data += interp
    while len(data) % 8:
        data.append(0)

    # PT_INTERP phdr
    interp_ph = bytearray(e_phentsize)
    struct.pack_into("<I", interp_ph, 0, 3)  # PT_INTERP
    struct.pack_into("<I", interp_ph, 4, 4)  # PF_R
    struct.pack_into("<Q", interp_ph, 8, interp_off)
    struct.pack_into("<Q", interp_ph, 16, 0)  # vaddr unused
    struct.pack_into("<Q", interp_ph, 24, 0)
    struct.pack_into("<Q", interp_ph, 32, len(interp))
    struct.pack_into("<Q", interp_ph, 40, len(interp))
    struct.pack_into("<Q", interp_ph, 48, 1)

    new_phdrs = [interp_ph] + phdrs
    phoff = len(data)
    for ph in new_phdrs:
        if len(ph) < e_phentsize:
            ph.extend(b"\0" * (e_phentsize - len(ph)))
        data += ph[:e_phentsize]

    struct.pack_into("<Q", data, 32, phoff)  # e_phoff
    struct.pack_into("<H", data, 56, len(new_phdrs))  # e_phnum
    if args.et_dyn:
        struct.pack_into("<H", data, 16, 3)  # ET_DYN
    # else keep original type

    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
    open(args.output, "wb").write(data)
    print(
        f"wrote {args.output}: interp={interp_path!r} phnum={len(new_phdrs)} "
        f"et_dyn={args.et_dyn} entry=0x{e_entry:x}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
