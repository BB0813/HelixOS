# Linux Syscall Compatibility

> M5：按 **helixbox**（BusyBox 风格 multi-call，自研 MIT）依赖扩展。完整 Linux ABI **未**宣称。

## Policy

- **主表**：x86_64 Linux syscall 编号。
- **未知号**：返回 `-ENOSYS`（38）。
- **诚实 `uname`**：`sysname=Helix`（**不是** Linux）。若将来伪装须在本文标明开关。
- **用户输出**：console `write` → 串口，每调用前缀 `[user] `。
- **FS**：`/` = FAT **只读**；`/tmp` = **ramfs 可写**（mkdir/create/write）。

## Table (x86_64) — Helix M5

| NR | Name | Status | Notes |
|----|------|--------|-------|
| 0 | read | **done** | FD；文件 VFS；console→0 |
| 1 | write | **done** | console 行缓冲+`[user] `；**ramfs 文件可写**；FAT 写失败 |
| 2 | open | **done** | 支持 O_CREAT/TRUNC/APPEND（ramfs）；FAT 只读 |
| 3 | close | **done** | |
| 5 | fstat | **done** | 最小 `struct stat` |
| 12 | brk | **done** | 按页扩展用户 brk |
| 16 | ioctl | **partial** | console 成功 stub |
| 24 | sched_yield | **done** | 协作 |
| 33 | dup2 | **done** | 无 refcount |
| 39 | getpid | **done** | |
| 60 | exit | **done** | |
| 63 | uname | **done** | sysname **Helix** |
| 72 | fcntl | **partial** | GETFD/GETFL stub 0 |
| 79 | getcwd | **done** | 恒为 `"/"` |
| 83 | mkdir | **done** | **仅 `/tmp/...` ramfs**；FAT 上失败 |
| 217 | getdents64 | **done** | 根目录 FAT 或 /tmp |

Entry：`syscall`/`sysretq`。Args：`rax` + `rdi,rsi,rdx,r10,r8,r9`。

## Applets (M5)

| 命令 | 实现 |
|------|------|
| echo / cat / ls / uname | `user/helixbox.c` → `/bin/helixbox` |
| sh -c 'echo …' | 极简 |
| smoke | 一次跑完上述 |

**不是 BusyBox**（本机构建无 Linux musl 交叉链）。接口为 Linux syscall ABI，便于日后替换真实静态 BusyBox。见 `third_party/README.md`。

## FS

| 路径 | 内容 |
|------|------|
| `/hello.txt` | `HelixFS OK` |
| `/bin/init.elf` `task2.elf` | M3 演示 |
| `/bin/helixbox` | M5 multi-call |

## Changelog

| 日期 | 变更 |
|------|------|
| 2026-07-28 | M3–M4 基础 |
| 2026-07-28 | M5 helixbox + uname/getdents/brk/… |
| 2026-07-28 | M5+ ramfs `/tmp`、mkdir、文件 write、行缓冲 console |
