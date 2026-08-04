#!/usr/bin/env python3
"""Build a GPT disk image with one FAT EFI System Partition containing BOOTX64.EFI."""
from __future__ import annotations

import argparse
import os
import struct
import sys
import uuid
import zlib


def u16(x: int) -> bytes:
    return struct.pack("<H", x & 0xFFFF)


def u32(x: int) -> bytes:
    return struct.pack("<I", x & 0xFFFFFFFF)


def u64(x: int) -> bytes:
    return struct.pack("<Q", x & 0xFFFFFFFFFFFFFFFF)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def encode_83(name: str) -> bytes:
    name = name.replace("\\", "/").split("/")[-1]
    if name in (".", ".."):
        base, ext = name, ""
    elif "." in name and not name.startswith("."):
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    return (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode("ascii", "replace")


def build_fat16_volume(size_bytes: int, files: list[tuple[str, bytes]]) -> bytes:
    """Return a standalone FAT16 volume (no partition table) of exact size_bytes."""
    sector = 512
    if size_bytes % sector:
        raise SystemExit("FAT size must be multiple of 512")
    total_sectors = size_bytes // sector
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_sectors = (root_entries * 32 + sector - 1) // sector
    spc = 8  # 4 KiB clusters — good for ~30MiB ESP
    fat_sectors = 1
    for _ in range(6):
        data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
        clusters = max(data_sectors // spc, 1)
        fat_sectors = (clusters * 2 + sector - 1) // sector
    data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
    clusters = data_sectors // spc
    if clusters < 2:
        raise SystemExit("ESP too small")

    bpc = spc * sector
    fat = bytearray(fat_sectors * sector)
    fat[0:4] = b"\xF8\xFF\xFF\xFF"

    def fat_set(cl: int, val: int) -> None:
        fat[cl * 2 : cl * 2 + 2] = u16(val)

    next_free = 2
    data = bytearray(clusters * bpc)

    def alloc_chain(nbytes: int) -> int:
        nonlocal next_free
        ncl = max((nbytes + bpc - 1) // bpc, 1)
        first = next_free
        for i in range(ncl):
            cl = next_free
            next_free += 1
            if (cl - 2) >= clusters:
                raise SystemExit("out of clusters in ESP")
            fat_set(cl, 0xFFFF if i == ncl - 1 else cl + 1)
        return first

    def write_chain(first: int, blob: bytes) -> None:
        cl, off = first, 0
        while True:
            start = (cl - 2) * bpc
            chunk = blob[off : off + bpc]
            data[start : start + len(chunk)] = chunk
            val = fat[cl * 2] | (fat[cl * 2 + 1] << 8)
            if val >= 0xFFF8:
                break
            cl, off = val, off + bpc

    def dir_entry(name83: bytes, attr: int, cluster: int, size: int) -> bytes:
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        e[26:28] = u16(cluster)
        e[28:32] = u32(size)
        return bytes(e)

    tree: dict = {}

    def ensure_dir(parts: list[str]) -> dict:
        node = tree
        for p in parts:
            if p not in node:
                node[p] = ("dir", {})
            kind, payload = node[p]
            if kind != "dir":
                raise SystemExit(f"path conflict: {p}")
            node = payload
        return node

    for dest, content in files:
        parts = [p for p in dest.replace("\\", "/").split("/") if p]
        *dirs, fname = parts
        ensure_dir(dirs)[fname] = ("file", content)

    def materialize_dir(node: dict, parent_cl: int, self_cl: int) -> None:
        entries = [
            dir_entry(encode_83("."), 0x10, self_cl, 0),
            dir_entry(encode_83(".."), 0x10, parent_cl, 0),
        ]
        children = []
        for name, (kind, payload) in sorted(node.items(), key=lambda x: x[0].upper()):
            if kind == "dir":
                cl = alloc_chain(bpc)
                children.append((name, "dir", cl, 0, payload))
            else:
                content = payload
                cl = alloc_chain(len(content) if content else 1)
                write_chain(cl, content)
                children.append((name, "file", cl, len(content), None))
        for name, kind, cl, size, _ in children:
            entries.append(dir_entry(encode_83(name), 0x10 if kind == "dir" else 0x20, cl, size))
        blob = b"".join(entries) + b"\x00" * 32
        if len(blob) > bpc:
            raise SystemExit("directory exceeds one cluster (M0 limit)")
        write_chain(self_cl, blob.ljust(bpc, b"\x00"))
        for name, kind, cl, size, sub in children:
            if kind == "dir":
                materialize_dir(sub, self_cl, cl)

    root = bytearray(root_sectors * sector)
    rpos = 0

    def root_add(e: bytes) -> None:
        nonlocal rpos
        root[rpos : rpos + 32] = e
        rpos += 32

    for name, (kind, payload) in sorted(tree.items(), key=lambda x: x[0].upper()):
        if kind == "dir":
            cl = alloc_chain(bpc)
            root_add(dir_entry(encode_83(name), 0x10, cl, 0))
            materialize_dir(payload, 0, cl)
        else:
            cl = alloc_chain(len(payload) if payload else 1)
            write_chain(cl, payload)
            root_add(dir_entry(encode_83(name), 0x20, cl, len(payload)))

    bpb = bytearray(sector)
    bpb[0:3] = b"\xEB\x3C\x90"
    bpb[3:11] = b"HELIXOS "
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
    bpb[24:26] = u16(32)
    bpb[26:28] = u16(64)
    bpb[54:62] = b"FAT16   "
    bpb[510:512] = b"\x55\xAA"

    img = bytearray()
    img += bpb
    img += fat * num_fats
    img += root
    img += data
    if len(img) < size_bytes:
        img += b"\x00" * (size_bytes - len(img))
    return bytes(img[:size_bytes])


def build_fat32_volume(size_bytes: int, files: list[tuple[str, bytes]]) -> bytes:
    """M21: FAT32 volume for >32 MiB ESP. Root lives in cluster chain (no fixed region)."""
    sector = 512
    if size_bytes % sector:
        raise SystemExit("FAT size must be multiple of 512")
    total_sectors = size_bytes // sector
    reserved = 32  # FAT32 needs space for FSInfo (sec 1) + backup boot (sec 6)
    num_fats = 2
    spc = 8  # 4 KiB clusters
    fat_sectors = 1
    for _ in range(6):
        # FAT32 has NO fixed root region: data starts right after FATs
        data_sectors = total_sectors - reserved - num_fats * fat_sectors
        clusters = max(data_sectors // spc, 1)
        fat_sectors = (clusters * 4 + sector - 1) // sector
    data_sectors = total_sectors - reserved - num_fats * fat_sectors
    clusters = data_sectors // spc
    if clusters < 65525:
        # Spec says FAT32 requires >= 65525 clusters. Caller should size accordingly.
        # We don't strictly enforce — Helix kernel will still mount and work.
        pass

    bpc = spc * sector
    fat = bytearray(fat_sectors * sector)
    fat[0:4] = b"\xF8\xFF\xFF\xFF"
    fat[4:8] = b"\xFF\xFF\xFF\xFF"
    fat[8:12] = b"\x0F\xFF\xFF\xFF"  # FSInfo: next free hint (lo) — not strictly required

    def fat_set(cl: int, val: int) -> None:
        fat[cl * 4 : cl * 4 + 4] = u32(val & 0xFFFFFFFF)

    next_free = 2
    data = bytearray(clusters * bpc)

    def alloc_chain(nbytes: int) -> int:
        nonlocal next_free
        ncl = max((nbytes + bpc - 1) // bpc, 1)
        first = next_free
        for i in range(ncl):
            cl = next_free
            next_free += 1
            if (cl - 2) >= clusters:
                raise SystemExit("out of clusters in ESP")
            fat_set(cl, 0x0FFFFFF8 if i == ncl - 1 else cl + 1)
        return first

    def write_chain(first: int, blob: bytes) -> None:
        cl, off = first, 0
        while True:
            start = (cl - 2) * bpc
            chunk = blob[off : off + bpc]
            data[start : start + len(chunk)] = chunk
            val = fat[cl * 4] | (fat[cl * 4 + 1] << 8) | (fat[cl * 4 + 2] << 16) | (fat[cl * 4 + 3] << 24)
            if val >= 0x0FFFFFF8:
                break
            cl, off = val, off + bpc

    def dir_entry(name83: bytes, attr: int, cluster: int, size: int) -> bytes:
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        e[20:22] = u16((cluster >> 16) & 0xFFFF)  # FAT32 high cluster
        e[26:28] = u16(cluster & 0xFFFF)
        e[28:32] = u32(size)
        return bytes(e)

    tree: dict = {}

    def ensure_dir(parts: list[str]) -> dict:
        node = tree
        for p in parts:
            if p not in node:
                node[p] = ("dir", {})
            kind, payload = node[p]
            if kind != "dir":
                raise SystemExit(f"path conflict: {p}")
            node = payload
        return node

    for dest, content in files:
        parts = [p for p in dest.replace("\\", "/").split("/") if p]
        *dirs, fname = parts
        ensure_dir(dirs)[fname] = ("file", content)

    root_cl = alloc_chain(bpc)  # FAT32 root is a regular cluster chain

    def materialize_dir(node: dict, parent_cl: int, self_cl: int, is_root: bool = False) -> None:
        entries = [] if is_root else [
            dir_entry(encode_83("."), 0x10, self_cl, 0),
            dir_entry(encode_83(".."), 0x10, parent_cl, 0),
        ]
        children = []
        for name, (kind, payload) in sorted(node.items(), key=lambda x: x[0].upper()):
            if kind == "dir":
                cl = alloc_chain(bpc)
                children.append((name, "dir", cl, 0, payload))
            else:
                content = payload
                cl = alloc_chain(len(content) if content else 1)
                write_chain(cl, content)
                children.append((name, "file", cl, len(content), None))
        for name, kind, cl, size, _ in children:
            entries.append(dir_entry(encode_83(name), 0x10 if kind == "dir" else 0x20, cl, size))
        blob = b"".join(entries) + b"\x00" * 32
        if len(blob) > bpc:
            raise SystemExit("directory exceeds one cluster")
        write_chain(self_cl, blob.ljust(bpc, b"\x00"))
        for name, kind, cl, size, sub in children:
            if kind == "dir":
                materialize_dir(sub, self_cl, cl, is_root=False)

    # D2: root also goes through materialize_dir. Pass is_root=True so root
    # omits `.`/`..` entries (FAT spec forbids them on root). All children —
    # files AND subdirs — land in `root_cl`'s single cluster. Single-cluster
    # overflow still raises (matches prior behavior); nested dirs recurse
    # and extend their own chain.
    materialize_dir(tree, 0, root_cl, is_root=True)

    bpb = bytearray(sector)
    bpb[0:3] = b"\xEB\x58\x90"
    bpb[3:11] = b"HELIXOS "
    bpb[11:13] = u16(sector)
    bpb[13] = spc
    bpb[14:16] = u16(reserved)
    bpb[16] = num_fats
    bpb[17:19] = u16(0)  # root_entries = 0 for FAT32
    bpb[19:21] = u16(0)  # tot16 = 0 (use tot32)
    bpb[21] = 0xF8
    bpb[22:24] = u16(0)  # fatsz16 = 0 (use fatsz32)
    bpb[24:26] = u16(32)
    bpb[26:28] = u16(64)
    bpb[32:36] = u32(total_sectors)
    bpb[36:40] = u32(fat_sectors)
    bpb[40:42] = u16(0)  # ext_flags
    bpb[42:44] = u16(0)  # fs_ver
    bpb[44:48] = u32(root_cl)
    bpb[48:50] = u16(1)  # fsinfo sector
    bpb[50:52] = u16(6)  # backup boot sector
    bpb[64] = 0x80  # drive number
    bpb[66] = 0x01
    bpb[82] = b"FAT32   "[0]  # not strictly required
    bpb[82:90] = b"FAT32   "
    bpb[510:512] = b"\x55\xAA"

    # FSInfo sector (sector 1)
    fsinfo = bytearray(sector)
    fsinfo[0:4] = b"\x52\x52\x61\x41"  # signature 1
    fsinfo[4:8] = b"\x00" * 4
    fsinfo[484:488] = b"\x72\x72\x41\x61"  # signature 2
    fsinfo[488:492] = u32(0xFFFFFFFF)  # free cluster count unknown
    fsinfo[492:496] = u32(0xFFFFFFFF)  # next free unknown
    fsinfo[510:512] = b"\x55\xAA"

    img = bytearray()
    img += bpb
    img += fsinfo
    img += b"\x00" * ((reserved - 2) * sector)  # sectors 2..reserved-1
    img += fat * num_fats
    img += data
    if len(img) < size_bytes:
        img += b"\x00" * (size_bytes - len(img))
    return bytes(img[:size_bytes])


def gpt_header(
    *,
    current_lba: int,
    backup_lba: int,
    first_usable: int,
    last_usable: int,
    disk_guid: uuid.UUID,
    part_lba: int,
    part_count: int,
    part_entry_size: int,
    part_array_crc: int,
) -> bytes:
    # EFI spec GPT header (92 bytes used, rest zero to sector size filled by caller)
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    h[8:12] = u32(0x00010000)  # revision
    h[12:16] = u32(92)  # header size
    h[16:20] = u32(0)  # crc32 placeholder
    h[20:24] = u32(0)
    h[24:32] = u64(current_lba)
    h[32:40] = u64(backup_lba)
    h[40:48] = u64(first_usable)
    h[48:56] = u64(last_usable)
    h[56:72] = disk_guid.bytes_le
    h[72:80] = u64(part_lba)
    h[80:84] = u32(part_count)
    h[84:88] = u32(part_entry_size)
    h[88:92] = u32(part_array_crc)
    c = crc32(bytes(h))
    h[16:20] = u32(c)
    return bytes(h)


def build_gpt_disk(esp_fat: bytes, total_mib: int) -> bytes:
    sector = 512
    total_sectors = total_mib * 1024 * 1024 // sector
    # Layout:
    # LBA0 protective MBR
    # LBA1 primary GPT header
    # LBA2..33 partition entries (32 * 128 = 4KiB, 8 LBAs actually enough; use 32 LBAs standard)
    entries_lbas = 32  # 16 KiB / 128B entries = 128 entries capacity; we use standard 32 LBA = 128 entries?
    # 32 LBAs * 512 = 16384; entry 128B → 128 entries. UEFI requires at least 128 entries.
    part_entry_size = 128
    part_count = 128
    first_usable = 1 + 1 + entries_lbas  # 34
    last_usable = total_sectors - 1 - entries_lbas - 1  # backup entries + backup header

    # ESP: start at LBA 2048 (1 MiB align), size = fit fat
    esp_start = 2048
    esp_sectors = len(esp_fat) // sector
    esp_end = esp_start + esp_sectors - 1
    if esp_end > last_usable:
        raise SystemExit("disk too small for ESP")

    disk_guid = uuid.UUID("A5F4C3B2-1000-4E11-8E11-48454C495831")  # stable for reproducibility
    esp_guid = uuid.UUID("A5F4C3B2-1001-4E11-8E11-48454C495831")
    # ESP type GUID
    esp_type = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")

    # partition entry array
    entries = bytearray(part_count * part_entry_size)
    ent = bytearray(part_entry_size)
    ent[0:16] = esp_type.bytes_le
    ent[16:32] = esp_guid.bytes_le
    ent[32:40] = u64(esp_start)
    ent[40:48] = u64(esp_end)
    ent[48:56] = u64(0)  # attributes; bit1=legacy boot sometimes, 0 fine for UEFI
    # name UTF-16LE "Helix ESP"
    name = "Helix ESP".encode("utf-16le")
    ent[56 : 56 + len(name)] = name
    entries[0:part_entry_size] = ent
    part_crc = crc32(bytes(entries))

    primary = gpt_header(
        current_lba=1,
        backup_lba=total_sectors - 1,
        first_usable=first_usable,
        last_usable=last_usable,
        disk_guid=disk_guid,
        part_lba=2,
        part_count=part_count,
        part_entry_size=part_entry_size,
        part_array_crc=part_crc,
    )
    backup = gpt_header(
        current_lba=total_sectors - 1,
        backup_lba=1,
        first_usable=first_usable,
        last_usable=last_usable,
        disk_guid=disk_guid,
        part_lba=total_sectors - 1 - entries_lbas,
        part_count=part_count,
        part_entry_size=part_entry_size,
        part_array_crc=part_crc,
    )

    # Protective MBR
    mbr = bytearray(sector)
    mbr[0:446] = b"\x00" * 446
    # one protective partition
    part = bytearray(16)
    part[0] = 0x00  # not bootable
    part[1:4] = b"\x00\x02\x00"  # CHS start dummy
    part[4] = 0xEE  # GPT protective
    part[5:8] = b"\xFF\xFF\xFF"
    part[8:12] = u32(1)  # start LBA
    part[12:16] = u32(min(total_sectors - 1, 0xFFFFFFFF))
    mbr[446:462] = part
    mbr[510:512] = b"\x55\xAA"

    img = bytearray(total_sectors * sector)
    img[0:512] = mbr
    img[512 : 512 + 92] = primary
    img[2 * sector : 2 * sector + len(entries)] = entries
    img[esp_start * sector : esp_start * sector + len(esp_fat)] = esp_fat
    backup_entries_lba = total_sectors - 1 - entries_lbas
    img[backup_entries_lba * sector : backup_entries_lba * sector + len(entries)] = entries
    img[(total_sectors - 1) * sector : (total_sectors - 1) * sector + 92] = backup
    return bytes(img)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--efi", required=True, help="path to BOOTX64.EFI")
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk-mib", type=int, default=64)
    ap.add_argument("--esp-mib", type=int, default=32)
    ap.add_argument(
        "--add",
        action="append",
        default=[],
        help="hostpath:DEST/IN/ESP  (repeatable; DEST uses / separators)",
    )
    ap.add_argument(
        "--add-tree",
        action="append",
        default=[],
        help="hostdir:DEST_PREFIX  (repeatable; recursively walks hostdir, strips hostdir prefix from each path; DEST_PREFIX may be '' for root)",
    )
    ap.add_argument(
        "--raw-fat",
        action="store_true",
        help="write only the FAT volume (no GPT/MBR). For mtools verification.",
    )
    args = ap.parse_args()

    with open(args.efi, "rb") as f:
        efi = f.read()

    files: list[tuple[str, bytes]] = [("EFI/BOOT/BOOTX64.EFI", efi)]
    for item in args.add:
        if "::" in item:
            host, dest = item.split("::", 1)
        else:
            # Windows drive paths: split on last colon that starts DEST
            host, dest = item.rsplit(":", 1)
        with open(host, "rb") as f:
            data = f.read()
        files.append((dest.replace("\\", "/"), data))
        print(f"  + {dest} ({len(data)} bytes from {host})")

    for item in args.add_tree:
        if "::" in item:
            host_root, dest_prefix = item.split("::", 1)
        else:
            # host:dest syntax — split on last ':'
            if ":" in item:
                host_root, dest_prefix = item.rsplit(":", 1)
            else:
                host_root, dest_prefix = item, ""
        host_root = host_root.rstrip("/\\")
        dest_prefix = dest_prefix.replace("\\", "/").strip("/")
        for dirpath, dirnames, filenames in os.walk(host_root):
            rel = os.path.relpath(dirpath, host_root).replace("\\", "/")
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                with open(full, "rb") as f:
                    data = f.read()
                if rel == ".":
                    dest = f"{dest_prefix}/{fn}" if dest_prefix else fn
                else:
                    dest = f"{dest_prefix}/{rel}/{fn}" if dest_prefix else f"{rel}/{fn}"
                files.append((dest, data))
                print(f"  + {dest} ({len(data)} bytes from {full})")

    esp_size = args.esp_mib * 1024 * 1024
    # M21: FAT32 once clusters would exceed 65524 (FAT16 limit). With spc=8,
    # that's roughly >32 MiB. Choose FAT32 conservatively at >=64 MiB.
    if esp_size >= 64 * 1024 * 1024:
        print(f"mkdisk: ESP {args.esp_mib} MiB → FAT32")
        fat = build_fat32_volume(esp_size, files)
    else:
        fat = build_fat16_volume(esp_size, files)
    if args.raw_fat:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "wb") as f:
            f.write(fat)
        print(
            f"wrote {args.out}: raw FAT volume ESP={args.esp_mib}MiB "
            f"files={len(files)} bytes={len(fat)}"
        )
        return 0
    disk = build_gpt_disk(fat, args.disk_mib)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(disk)
    print(
        f"wrote {args.out}: disk={args.disk_mib}MiB ESP={args.esp_mib}MiB "
        f"files={len(files)} efi={len(efi)} bytes"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
