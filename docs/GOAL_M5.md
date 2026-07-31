# Goal: HelixOS M5 — Linux 兼容子集（历史）

> **状态：已完成（2026-07-28 起）** — helixbox + 可选 BusyBox；`make smoke-linux`。  
> 注：早期 goal 文中「mkdir 未做」已过时——`mkdir`(83) 与 `/tmp` ramfs 已在本阶段落地。

## 范围

1. 扩展 syscall（uname/getdents64/brk/fstat/open/read/write/…）；未知 → `-ENOSYS`。
2. **helixbox** multi-call：echo/cat/ls/uname/sh -c/smoke。
3. `uname` → **Helix**（诚实，非伪装 Linux）。
4. 可选 `/bin/busybox`（`third_party/`，GPL）：`echo HelixBusyBoxOK`。
5. `/tmp` ramfs 可写 + `mkdir`(83)；console 行缓冲 `[user] `。

## 实现

`user/helixbox.c`、`kernel/proc/{syscall,exec}.c`、FAT `getdents64`、`kernel/fs/ramfs.c`。

## 验收

`make smoke-linux`：BusyBox 和/或 helixbox 标记（`HelixBusyBoxOK` / `HelixLinuxOK` / `helixbox_smoke_done`）。

## 后续

M6 动态链接；M8+ 网络 applet；M10–M12 进程/管道/cwd 并入同一 smoke 链。
