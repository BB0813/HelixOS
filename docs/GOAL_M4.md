# Goal: HelixOS M4 — VFS + 存储

> **状态：已完成（2026-07-28，只读）** — AHCI `blk_read`、GPT+FAT16、VFS `/`、FD、盘上 ELF；  
> 验证：`make smoke-fs`。写路径未做。本文件保留为历史 goal。

实现：`kernel/drv/blk_ahci.c`、`kernel/fs/{fat,vfs,fs}.c`；`scripts/mkdisk.py --add`。
