# Goal: HelixOS M4 — VFS + 存储（历史）

> **状态：已完成**  
> - 初版只读（2026-07-28）：AHCI `blk_read`、GPT+FAT16、VFS `/`、FD、盘上 ELF → `make smoke-fs`。  
> - 写路径补全（2026-07 末）：AHCI `blk_write`、FAT16 根 create/write/mkdir 持久化 → **`HelixFATWriteOK`**（`/HELIXW.TXT`）。

## 实现

- `kernel/drv/blk_ahci.c`：port0 同步 READ/WRITE DMA EXT；MMIO map  
- `kernel/fs/{fat,vfs,fs,ramfs}.c`：`/` FAT16 RW；`/tmp` ramfs  
- `scripts/mkdisk.py --add` / `mkesp`：hello.txt、bin/*.elf

## 验收

`make smoke-fs`：`HelixFS OK` · **`HelixFATWriteOK`** · `FAT / RW`。

## 后续相关

M12 为相对路径与 cwd；FAT32 写 / 深层子目录仍非目标（见 ROADMAP 债）。
