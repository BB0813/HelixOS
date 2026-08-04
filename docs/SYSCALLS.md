# Linux Syscall Compatibility

> M5/M6：helixbox + 可选静态 BusyBox；动态 `ld-helix` / 真 musl。  
> M7：内核 ICMP 自测（**无** socket 类 syscall）。完整 Linux ABI **未**宣称。  
> M10：`fork`/`execve` 实现，进程创建与 ELF 加载。

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
| 9 | mmap | **partial** | **匿名**（`MAP_ANONYMOUS` 或 `fd==-1`）；支持 `addr=0` 与 FIXED/hint；**非** file-backed；M18：**`fd==-4`** → map GOP framebuffer 物理页 |
| 10 | mprotect | **partial** | 成功 stub（不改页属性） |
| 11 | munmap | **partial** | 成功 stub（暂泄漏页） |
| 12 | brk | **done** | 按页扩展 |
| 13 | rt_sigaction | **done** | SIG_DFL/IGN/handler 存表；用户 handler 帧未做 |
| 14 | rt_sigprocmask | **done** | block/unblock/set；不可 mask KILL/STOP |
| 16 | ioctl | **partial** | console stub |
| 24 | sched_yield | **done** | 协作 |
| 33 | dup2 | **done** | |
| 39 | getpid | **done** | |
| 41 | socket | **done** | AF_INET；SOCK_DGRAM→UDP，SOCK_STREAM→TCP（M14） |
| 42 | connect | **done** | TCP active open（SYN_SENT→EST）（M14） |
| 43 | accept | **done** | TCP passive accept（M14） |
| 44 | sendto | **done** | TCP(PSH+ACK via `tcp_send_data`) / UDP 共路径，路由 by `is_socket` type（M15） |
| 45 | recvfrom | **done** | TCP(`tcp_recv_data`) / UDP 共路径（非阻塞），路由 by `is_socket` type（M15） |
| 46 | sendmsg | **done** | iovec coalesce → sendto；TCP/UDP 路由（M16） |
| 47 | recvmsg | **done** | recvfrom → iovec scatter；TCP/UDP 路由（M16） |
| 49 | bind | **done** | TCP / UDP 共路径（is_socket type） |
| 50 | listen | **done** | TCP LISTEN + accept backlog（M14） |
| 54 | setsockopt | **soft-stub** | 返回 0（TCP_NODELAY etc.） |
| 55 | getsockopt | **soft-stub** | 返回 0 |
| 57 | fork | **done** | 复制 task + 用户页表（独立 PML4） |
| 59 | execve | **done** | VFS 加载 ELF，替换用户空间；argv 支持 |
| 60 | exit | **done** | 关闭继承 FD（除 console） |
| 61 | wait4 | **done** | 非阻塞轮询；`(code&0xFF)<<8`；reap zombie |
| 62 | kill | **done** | 正 pid / 0=self；SIGKILL 不可 mask；投递后 deliver |
| 63 | uname | **done** | sysname **Helix** |
| 72 | fcntl | **partial** | stub |
| 79 | getcwd | **done** | 每任务真实 cwd（默认 `"/"`） |
| 80 | chdir | **done** | 路径解析 + 目录校验；fork 继承 |
| 83 | mkdir | **done** | **`/tmp/...` 与 FAT 根 8.3**；相对路径经 cwd |
| 22 | pipe | **done** | 环形缓冲；EAGAIN 非阻塞 |
| 158 | arch_prctl | **partial** | `ARCH_SET_FS` / `GET_FS`（MSR） |
| 217 | getdents64 | **done** | FAT 根或 /tmp |
| 231 | exit_group | **done** | |
| 257 | openat | **done** | 重定向至 open |
| 262 | newfstatat | **done** | 重定向至 fstatat |
| 318 | getrandom | **done** | 软实现（ticks） |
| 546 | sys_fb_info (custom) | **done** | M18：返回 `{width, height, pitch, bpp, size}` |
| 547 | sys_readkey (custom) | **done** | M18：非阻塞 PS/2 键盘读取 |

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
| `/bin/msh` | M11 用户态 shell (fork/exec/wait/pipe) |
| `/bin/tui` | M19 用户态 TUI shell（fb mmap + PS/2 键盘） |

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
| 2026-08-01 | M10：`fork`/`execve`（独立 PML4 + 页表递归复制）→ **`ForkChildOK`** |
| 2026-08-01 | M11：`wait4`/`pipe`/`dup2` refcount + syscall 全寄存器切换 → **`PipeOK`**/**`WaitOK`**；**msh** shell → **`HelixMshOK`** |
| 2026-08-01 | M12：per-task cwd + `chdir`(80) + 路径解析；console stdin poll；msh cd/pwd → **`HelixCwdOK`** |
| 2026-08-01 | M13：信号最小集（SIGCHLD/SIGINT/kill + HelixSigOK） → **`HelixSigOK`** |
| 2026-08-01 | M14：TCP 全栈（state machine + connect/accept/listen + is_socket=2 + kernel self-test） → **`HelixTcpOK`** |
| 2026-08-01 | M15：sendto/recvfrom TCP 路由（is_socket==2 → tcp_send_data/tcp_recv_data）；helixbox TCP echo → **`HelixTcpUserOK`** |
| 2026-08-01 | M16：sendmsg(46)/recvmsg(47) 完整实现（iovec coalesce/scatter + TCP/UDP 路由） |
| 2026-08-01 | 文档对齐 M0–M15（README/ARCHITECTURE/BUILD/GOAL_*）；下一候选 M16 TCP passive + sendmsg |
| 2026-08-04 | M16 TCP passive：pending child（无 listener 早期 SYN 入队）+ listen() adopt；helixbox 双进程（fork 被动 + 主 active） → **`HelixTcpPassiveOK`** |
| 2026-08-04 | M17 TCP 重传：TXQ 入队 + ACK 清除 + `tcp_retransmit()` 每 net_poll 调用；SYN_SENT/ESTABLISHED/FIN_WAIT_1 定时重发 |
| 2026-08-04 | **修复**：tcp_init 不再 wipe 已 used 槽位（保留 pending child，否则被 active socket 复用导致 adopt 失败） |
| 2026-08-04 | M18 fb user-space：`sys_mmap(fd=-4)` 映射 GOP framebuffer；`sys_fb_info`(546) 返回分辨率；`sys_readkey`(547) PS/2 键盘非阻塞读；helixbox `HelixFBInfoOK`/`HelixFBMmapOK`/`HelixKbOK` |
| 2026-08-04 | M19 TUI shell：用户态 mini-terminal（`bin/tui`）使用 fb mmap + PS/2 键盘；8x16 VGA 字体；内置 help/clear/echo/ls/cat/ps/tcpstat/time/reboot/exit；内核 shell `tui` 命令启动 |

