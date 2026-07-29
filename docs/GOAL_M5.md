# Goal: HelixOS M5 — Linux 兼容子集

> **状态：已完成（2026-07-28，helixbox 等价路径）**  
> `make smoke-linux` 通过；`uname`→Helix；真实 BusyBox 步骤见 `third_party/README.md`；`mkdir` 未做。

实现：`user/helixbox.c`、`kernel/proc/{syscall,exec}.c`、FAT `getdents64`。
