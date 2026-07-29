#!/usr/bin/env python3
"""Minimal FAT16 disk image builder for HelixOS ESP (no external deps)."""
from __future__ import annotations

import argparse
import os
import struct
import sys


def u8(x: int) -> bytes:
    return struct.pack("<B", x & 0xFF)


def u16(x: int) -> bytes:
    return struct.pack("<H", x & 0xFFFF)


def u32(x: int) -> bytes:
    return struct.pack("<I", x & 0xFFFFFFFF)


def encode_83(name: str, is_dir: bool) -> bytes:
    """Encode a single path component as 8.3 (uppercased, lossy)."""
    name = name.replace("\\", "/").split("/")[-1]
    if name in (".", ".."):
        base, ext = name, ""
    else:
        if "." in name and not name.startswith("."):
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
    base = base.upper()[:8].ljust(8)
    ext = ext.upper()[:3].ljust(3)
    # pad with spaces (ASCII)
    out = (base + ext).encode("ascii", errors="replace")
    assert len(out) == 11
    return out


def build_fat16(size_mib: int, files: list[tuple[str, bytes]]) -> bytes:
    sector = 512
    total_sectors = size_mib * 1024 * 1024 // sector
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_sectors = (root_entries * 32 + sector - 1) // sector
    # Choose sectors_per_cluster so data clusters fit FAT16 range
    spc = 4  # 2 KiB clusters — fine for 32MiB
    # FAT size: each entry 2 bytes; estimate clusters then recompute
    # rough: data_sectors ≈ total - reserved - fats - root
    # iterate once
    fat_sectors = 1
    for _ in range(4):
        data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
        clusters = max(data_sectors // spc, 1)
        fat_sectors = (clusters * 2 + sector - 1) // sector
    data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
    clusters = data_sectors // spc
    if clusters < 0xFFF:
        # still ok as FAT16 with small count; OVMF accepts
        pass
    if clusters >= 0xFFF5:
        raise SystemExit("image too large for this simple FAT16 builder")

    bytes_per_cluster = spc * sector
    fat = bytearray(fat_sectors * sector)
    # cluster 0 media, cluster 1 EOC
    fat[0] = 0xF8
    fat[1] = 0xFF
    fat[2] = 0xFF
    fat[3] = 0xFF

    def fat_set(cl: int, val: int) -> None:
        off = cl * 2
        fat[off : off + 2] = u16(val)

    next_free = 2
    data = bytearray(clusters * bytes_per_cluster)

    def alloc_chain(nbytes: int) -> int:
        nonlocal next_free
        ncl = max((nbytes + bytes_per_cluster - 1) // bytes_per_cluster, 1)
        first = next_free
        for i in range(ncl):
            cl = next_free
            next_free += 1
            if next_free > clusters + 1:
                raise SystemExit("out of clusters")
            if i == ncl - 1:
                fat_set(cl, 0xFFFF)
            else:
                fat_set(cl, cl + 1)
        return first

    def write_cluster_chain(first: int, blob: bytes) -> None:
        cl = first
        off = 0
        while True:
            start = (cl - 2) * bytes_per_cluster
            chunk = blob[off : off + bytes_per_cluster]
            data[start : start + len(chunk)] = chunk
            off += bytes_per_cluster
            val = fat[cl * 2] | (fat[cl * 2 + 1] << 8)
            if val >= 0xFFF8:
                break
            cl = val

    # Directory helpers: each dir is a list of 32-byte entries, ends with 0
    def dir_entry(name83: bytes, attr: int, cluster: int, size: int) -> bytes:
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        e[26:28] = u16(cluster)
        e[28:32] = u32(size)
        return bytes(e)

    # Tree: path parts -> {name: ("dir", dict) | ("file", data)}
    tree: dict = {}

    def ensure_dir(path_parts: list[str]) -> dict:
        node = tree
        for p in path_parts:
            if p not in node:
                node[p] = ("dir", {})
            kind, payload = node[p]
            if kind != "dir":
                raise SystemExit(f"path conflict at {p}")
            node = payload
        return node

    for host_or_none, dest in []:
        pass

    for rel_dest, content in files:
        parts = [p for p in rel_dest.replace("\\", "/").split("/") if p]
        if not parts:
            continue
        *dirs, fname = parts
        parent = ensure_dir(dirs)
        parent[fname] = ("file", content)

    def materialize_dir(node: dict, parent_cluster: int, self_cluster: int) -> int:
        """Return cluster of this directory after writing entries + children."""
        # First allocate space for dir entries once we know count — two-pass:
        # create child dirs/files first to know their clusters, then write.
        entries: list[bytes] = []
        # . and ..
        entries.append(dir_entry(encode_83("."), 0x10, self_cluster, 0))
        entries.append(dir_entry(encode_83(".."), 0x10, parent_cluster, 0))

        # Pre-create subdir clusters
        child_meta = []  # (name, kind, cluster_or_none, size, subnode/content)
        for name, (kind, payload) in sorted(node.items(), key=lambda x: x[0].upper()):
            if kind == "dir":
                # allocate at least 1 cluster for subdir
                cl = alloc_chain(bytes_per_cluster)
                child_meta.append((name, "dir", cl, 0, payload))
            else:
                content: bytes = payload
                cl = alloc_chain(len(content)) if content else alloc_chain(1)
                write_cluster_chain(cl, content)
                child_meta.append((name, "file", cl, len(content), None))

        for name, kind, cl, size, sub in child_meta:
            attr = 0x10 if kind == "dir" else 0x20
            entries.append(dir_entry(encode_83(name), attr, cl, size))

        # end marker
        blob = b"".join(entries) + b"\x00" * 32
        # write this dir's cluster chain — may need to re-alloc if larger
        # self_cluster already allocated with 1 cluster; if need more, extend
        need = (len(blob) + bytes_per_cluster - 1) // bytes_per_cluster
        # simple: only support 1 cluster dirs for M0 (plenty for few files)
        if need > 1:
            raise SystemExit("directory too large for 1-cluster M0 builder")
        write_cluster_chain(self_cluster, blob.ljust(bytes_per_cluster, b"\x00"))

        for name, kind, cl, size, sub in child_meta:
            if kind == "dir":
                materialize_dir(sub, self_cluster, cl)
        return self_cluster

    # Root dir is fixed region, not a cluster — handle specially
    root = bytearray(root_sectors * sector)
    root_pos = 0

    def root_add(entry: bytes) -> None:
        nonlocal root_pos
        if root_pos + 32 > len(root):
            raise SystemExit("root directory full")
        root[root_pos : root_pos + 32] = entry
        root_pos += 32

    for name, (kind, payload) in sorted(tree.items(), key=lambda x: x[0].upper()):
        if kind == "dir":
            cl = alloc_chain(bytes_per_cluster)
            root_add(dir_entry(encode_83(name), 0x10, cl, 0))
            materialize_dir(payload, 0, cl)
        else:
            content = payload
            cl = alloc_chain(len(content) if content else 1)
            write_cluster_chain(cl, content)
            root_add(dir_entry(encode_83(name), 0x20, cl, len(content)))

    # BPB / boot sector
    bpb = bytearray(sector)
    bpb[0:3] = b"\xEB\x3C\x90"
    bpb[3:11] = b"MSWIN4.1"
    bpb[11:13] = u16(sector)
    bpb[13] = spc
    bpb[14:16] = u16(reserved)
    bpb[16] = num_fats
    bpb[17:19] = u16(root_entries)
    if total_sectors < 0x10000:
        bpb[19:21] = u16(total_sectors)
    else:
        bpb[19:21] = u16(0)
        bpb[32:36] = u32(total_sectors)
    bpb[21] = 0xF8
    bpb[22:24] = u16(fat_sectors)
    bpb[24:26] = u16(32)  # sect/track
    bpb[26:28] = u16(64)  # heads
    bpb[54:62] = b"FAT16   "
    bpb[510:512] = b"\x55\xAA"

    img = bytearray()
    img += bpb
    img += fat * num_fats
    img += root
    img += data
    # pad to exact size
    want = total_sectors * sector
    if len(img) < want:
        img += b"\x00" * (want - len(img))
    return bytes(img[:want])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size-mib", type=int, default=32)
    ap.add_argument("--out", required=True)
    ap.add_argument(
        "--add",
        action="append",
        default=[],
        help="hostpath:DEST/IN/IMAGE  (DEST uses / separators)",
    )
    args = ap.parse_args()

    files: list[tuple[str, bytes]] = []
    for item in args.add:
        if ":" not in item:
            print(f"bad --add {item!r}, want host:DEST", file=sys.stderr)
            return 2
        # split on last colon on Windows drive? support HOSTPATH::DEST or host=dest
        if "=" in item and ":" not in item.split("=", 1)[0]:
            host, dest = item.split("=", 1)
        else:
            # path may be C:/... — split on last ':' that starts DEST? use '::'
            if "::" in item:
                host, dest = item.split("::", 1)
            else:
                # last colon
                host, dest = item.rsplit(":", 1)
        with open(host, "rb") as f:
            files.append((dest, f.read()))

    img = build_fat16(args.size_mib, files)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)
    print(f"wrote {args.out} ({len(img)} bytes, {len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
