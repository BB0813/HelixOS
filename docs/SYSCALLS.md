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
  - `/` = **FAT16/FAT32 可写**（根目录 create/write/mkdir；AHCI 写回 ESP 镜像）
  - `/tmp` = **ramfs 可写**
  - FAT32: 挂载、写路径、stale-cleanup（M21 done）

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
| 7 | poll | **done** | M24：`helix_pollfd[]`；不阻塞（timeout 忽略）；POLLNVAL/POLLERR/POLLIN/POLLOUT |
| 271 | ppoll | **done** | M24：忽略 timespec/sigmask；委托 sys_poll |
| 74 | fsync | **done** | M24：验证 fd；当前 FAT/ramfs 同步 write，no-op |
| 75 | fdatasync | **done** | M24：同 fsync（无独立 metadata flush 路径） |
| 82 | rename | **done** | M24：同目录 only (cross-dir 留 M25+)，FAT + ramfs |
| 84 | rmdir | **done** | M24：目录必须空；FAT + ramfs |
| 87 | unlink | **done** | M24：FAT mark 0xE5 + fat_free_chain；ramfs node_release |
| 231 | exit_group | **done** | |
| 257 | openat | **done** | 重定向至 open |
| 262 | newfstatat | **done** | 重定向至 fstatat |
| 318 | getrandom | **done** | 软实现（ticks） |
| 90 | chmod | **ENOSYS** | M20 显式 -ENOSYS（无 kprintf 刷屏） |
| 91 | fchmod | **ENOSYS** | M20 |
| 92 | chown | **ENOSYS** | M20 |
| 93 | fchown | **ENOSYS** | M20 |
| 94 | lchown | **ENOSYS** | M20 |
| 21 | access | **ENOSYS** | M20 显式 -ENOSYS |
| 132 | utime | **ENOSYS** | M20 |
| 133 | utimes | **ENOSYS** | M20 |
| 280 | utimensat | **ENOSYS** | M20 |
| 546 | sys_fb_info (custom) | **done** | M18：返回 `{width, height, pitch, bpp, size}` |
| 547 | sys_readkey (custom) | **done** | M18：非阻塞 PS/2 键盘读取 |
| 548 | sys_mouse_read (custom) | **done** | M23：非阻塞 PS/2 鼠标读取，返回 `{dx, dy, buttons}` 数组；空时 -EAGAIN (-11) |

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
| 2026-08-04 | M21 FAT32 完善：`fat_free_chain` + `root_unlink_and_free` helper（FAT16 固定根 + FAT32 root cluster chain）；`fat_selftest_write` 改走 helper 修 stale-cleanup；`mkdisk.py` 新增 `build_fat32_volume`（ESP ≥ 64 MiB 自动选 FAT32）；mformat + mcopy 验证 64 MiB 大盘 FAT32 镜像两次连 boot → **`HelixFATWriteOK`**（cluster 3 重新分配） |
| 2026-08-04 | M22 抢占式调度：timer.c 加 `g_preempt_pending` 累加 + `timer_preempt_pending()` xchg-clear + `PREEMPT_THRESHOLD=8`；syscall.c 返回路径加抢占检查（gate 1: `task_count_alive()>1`, gate 2: 阈值），命中后 `task_yield()`；helixbox 加 `HelixPreemptOK` smoke marker（fork 心跳 child 写 20 dots）；smoke-fs 不回归，smoke-net HelixTcpUserOK/HelixTcpPassiveOK 不回归 |
| 2026-08-05 | M23 PS/2 鼠标：`kernel/drv/ps2.c` 抽 `ps2_write_cmd`/`ps2_write_aux`/`ps2_flush_data` helper；`ps2_init` 第二阶段启用 aux port + IRQ12 + 100Hz sample rate；`ps2_mouse_handler` 累积 3-byte packet → `{dx,dy,buttons}` ring buffer（y 轴翻转）；`sys_mouse_read`(548) 非阻塞 drain，空时 -EAGAIN；helixbox 加 `HelixMouseOK` smoke marker；smoke-fs EXIT=0 不回归 |
| 2026-08-05 | M24 POSIX file ops 收尾：`struct vfs_ops` 扩到 14 字段（加 poll/unlink/rmdir/rename/fsync）；`sys_poll`/`sys_ppoll`(7/271) + `sys_unlink`/`sys_rmdir`/`sys_rename`(87/84/82) + `sys_fsync`/`sys_fdatasync`(74/75)；`vfs_poll_one` default by file type；FAT `fat_resolve_parent` + `dir_unlink_at` (0xE5 + fat_free_chain) + `dir_rename_at` + `dir_is_empty`；ramfs `node_release` + `node_child_count` + `ramfs_unlink_op/rmdir_op/rename_op`；dispatch default 改 silent `ERR(ENOSYS)` (无 `[syscall] ENOSYS` 刷屏)；helixbox 加 `HelixPollOK` + `HelixUnlinkOK` + `HelixFsyncOK` smoke markers |
| 2026-08-05 | **路线 D 收尾**：D1 `Makefile` smoke-net 端口等待 + `HelixTcpUserOK`/`HelixTcpPassiveOK` 升 hard-fail + `kernel/net/tcp.c` max-retries log 节流 30s；D2 `scripts/mkdisk.py` FAT32 root 路径走 `materialize_dir` + `is_root` flag 修 nested dir bug（mtools mdir 验证 EFI/BOOT/BOOTX64.EFI 可达）；D3 `kernel/drv/ps2.c` 加 0xE0 prefix 翻译箭头键 ESC [ A/B/C/D + `user/msh.c` msh_readline 重写为 cursor + 16 history + Ctrl+A/E/W/U/C |
| 2026-08-05 | M20 VFS ext：`fat_getdents64` 改用 `fs_priv` 存 `fat_dir_iter` 走 cluster chain（subdir open 完整）；`fat_resolve` 加 `out_attr` 报告 leaf 是 dir；`fat_open` 检测 `attr & 0x10` 时返回 `is_dir=1` + `fs_priv=fat_dir_iter`；`syscall.c` 加 `case 21/90/91/92/93/94/132/133/280` 显式 -ENOSYS（不刷屏）；`mkdisk.py` 加 `--add-tree` + `--raw-fat` 标志；`mkdisk_deep.sh` 4 级目录验证 + mtools mdir 验证；helixbox `cmd_smoke` 加 subdir 探针 (`ls /etc` / `cat /etc/passwd` / `cat /etc/welcome.txt` / `ls /lib`) |
| 2026-08-05 | M20 userland：`kernel/proc/exec.c` `linux_compat_run_busybox_applets` 5-applet chain (`echo HelixBusyBoxOK` → `cat /etc/welcome.txt` → `echo BB2_OK` → `true` → `echo HELIX_BB_DONE`) + 模块级 `g_bb_idx` 状态 + 自递归 exit-all-hook；`kernel/mm/heap.c` HEAP_PAGES 1024→2048 (4→8 MiB) 容纳 3+ BusyBox ELF 重 load；`user/msh.c` 加 6 builtin (`alias`/`unalias`/`export`/`unset`/`test` 含 `[` 形式 /`type`) + 文件静态 `msh_aliases[16]` + `msh_envtab[32]` 表；`msh_exec_line` 拆为 line/pipeline/strtok_r 三层支持 `;` statement separator；alias expansion 在 `msh_exec_pipeline` 早段 (lookup → 拼 body+tail → 重 tokenize)；bi_test 修单参 `-f` 优先于二元 `=` 检查 |

