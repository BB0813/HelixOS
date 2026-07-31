# Linux Syscall Compatibility

> M5/M6：helixbox + 可选静态 BusyBox；动态 `ld-helix` / 真 musl。  
> M7：内核 ICMP 自测（**无** socket 类 syscall）。完整 Linux ABI **未**宣称。

## Policy

- **主表**：x86_64 Linux syscall 编号。
- **未知号**：返回 `-ENOSYS`（38）。
- **诚实 `uname`**：`sysname=Helix`（**不是** Linux）。
- **用户输出**：console `write` → 串口，行缓冲前缀 `[user] `。
- **FS**：
  - `/` = **FAT16 可写**（根目录 create/write/mkdir；AHCI 写回 ESP 镜像）
  - `/tmp` = **ramfs 可写**
  - FAT32 写路径未做

## Table (x86_64)

| NR | Name | Status | Notes |
|----|------|--------|-------|
| 0 | read | **done** | FD；文件 VFS；console→0 |
| 1 | write | **done** | console 行缓冲；**FAT 与 ramfs 文件可写** |
| 2 | open | **done** | O_CREAT/TRUNC/APPEND（FAT 根 + ramfs） |
| 3 | close | **done** | |
| 5 | fstat | **done** | 最小 `struct stat` |
| 9 | mmap | **partial** | **匿名**（`MAP_ANONYMOUS` 或 `fd==-1`）；支持 `addr=0` 与 FIXED/hint；**非** file-backed |
| 10 | mprotect | **partial** | 成功 stub（不改页属性） |
| 11 | munmap | **partial** | 成功 stub（暂泄漏页） |
| 12 | brk | **done** | 按页扩展 |
| 16 | ioctl | **partial** | console stub |
| 24 | sched_yield | **done** | 协作 |
| 33 | dup2 | **done** | |
| 39 | getpid | **done** | |
| 41 | socket | **done** | AF_INET / SOCK_DGRAM (UDP)；SOCK_STREAM → ENOSYS |
| 42 | connect | **ENOSYS** | TCP stub |
| 43 | accept | **ENOSYS** | TCP stub |
| 44 | sendto | **done** | UDP 发包；本地回环 |
| 45 | recvfrom | **done** | UDP 收包（非阻塞） |
| 46 | sendmsg | **ENOSYS** | TCP stub |
| 47 | recvmsg | **ENOSYS** | TCP stub |
| 49 | bind | **done** | UDP 端口绑定 |
| 50 | listen | **ENOSYS** | TCP stub |
| 54 | setsockopt | **ENOSYS** | TCP stub |
| 55 | getsockopt | **ENOSYS** | TCP stub |
| 60 | exit | **done** | |
| 63 | uname | **done** | sysname **Helix** |
| 72 | fcntl | **partial** | stub |
| 79 | getcwd | **done** | 恒 `"/"` |
| 83 | mkdir | **done** | **`/tmp/...` 与 FAT 根 8.3** |
| 158 | arch_prctl | **partial** | `ARCH_SET_FS` / `GET_FS`（MSR） |
| 217 | getdents64 | **done** | FAT 根或 /tmp |
| 231 | exit_group | **done** | |
| 257 | openat | **done** | 重定向至 open |
| 262 | newfstatat | **done** | 重定向至 fstatat |
| 318 | getrandom | **done** | 软实现（ticks） |

Entry：`syscall`/`sysretq`。Args：`rax` + `rdi,rsi,rdx,r10,r8,r9`。

## FS paths

| 路径 | 内容 |
|------|------|
| `/hello.txt` | `HelixFS OK`（构建放入） |
| `/HELIXW.TXT` | 启动自检写入 **`HelixFATWriteOK`** |
| `/bin/helixbox` | M5 multi-call（MIT） |
| `/bin/busybox` | 可选静态 BusyBox（GPL，fetch 脚本） |
| `/bin/hello.dyn` `/lib/ld-helix.so` | M6 最小动态（自研 ld-helix） |
| `/bin/hello.musl` `/lib/ld-musl-x86_64.so.1` `/lib/libc.so` | **真 musl** PIE + loader |

## Changelog

| 日期 | 变更 |
|------|------|
| 2026-07-28 | M3–M5；ramfs `/tmp` |
| 2026-07-29 | M6 `HelloDynOK`；BusyBox echo；FAT16 根写 |
| 2026-07-29 | M6 收尾：mmap 标志文档化；ARCHITECTURE/BUILD 动态路径 |
| 2026-07-31 | **真 musl** `ld-musl` + PIE hello → **`HelloMuslDynOK`**（`make smoke-musl`） |
| 2026-07-31 | M7：e1000 + ARP/IPv4/ICMP → **`HelixNetOK`**；socket 仍 ENOSYS |
| 2026-07-31 | M8：UDP socket（`socket`/`bind`/`sendto`/`recvfrom`）→ **`user_udp_ok`** |
| 2026-08-01 | M8 TCP stubs：`connect`/`accept`/`listen`/`sendmsg`/`recvmsg`/`setsockopt`/`getsockopt` → ENOSYS |
