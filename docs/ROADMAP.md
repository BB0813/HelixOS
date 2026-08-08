# Roadmap

按顺序推进；**完成一个里程碑的可构建/可验证增量再开下一个**。

状态：`[ ]` 未做 · `[~]` 进行中 · `[x]` 完成

---

## M0 — 仓库与工具链 `[x]`

Goal（历史）：[`docs/GOAL_M0.md`](GOAL_M0.md)

- [x] 项目命名确认：HelixOS / 内核 Helix
- [x] 目录结构、README、LICENSE（MIT）、`.gitignore`
- [x] `docs/ARCHITECTURE.md` `BUILD.md` `ROADMAP.md` `SYSCALLS.md`
- [x] 最小 freestanding UEFI 应用（打印 Helix boot）
- [x] Makefile + `scripts/`（deps / esp / qemu / mkdisk）
- [x] `make` 产出 `out/BOOTX64.EFI`（本机验证，MSYS2 clang+lld）
- [x] `make smoke` / QEMU+OVMF 串口可见横幅（本机验证）

**验证**：`make check-deps && make && make smoke`，`serial.log` 含 `Helix boot`。

---

## M1 — 启动与早期核 `[x]`

Goal（历史）：[`docs/GOAL_M1.md`](GOAL_M1.md)

- [x] UEFI：获取 MemoryMap（GOP 未做，M2+ 可选）
- [x] `ExitBootServices`（失败重试；成功后切自有栈）
- [x] Identity-map 页表（2MiB large pages，覆盖 RAM ceiling）
- [x] IDT + 异常 0–31 → `panic`（`#PF` 打印 CR2）
- [x] 物理页分配器（bitmap，仅 ConventionalMemory 为空闲）
- [x] 内核堆 `kmalloc`/`kfree`（256 KiB first-fit）
- [x] COM1 `kprintf` / `panic` / `assert`（BS 前后均可用）
- [x] 边界：`boot/` 只负责 UEFI 与 handoff；`kernel/` + `libk/` 为早期核

**验证**：`make smoke` 见 `ExitBootServices OK` + `M1 early kernel OK`；  
`make smoke-panic` 见 `HELIX PANIC` / `#PF`。

---

## M2 — 中断与内核壳 `[x]`

Goal 提示词（历史）：[`docs/GOAL_M2.md`](GOAL_M2.md)

- [x] 时钟中断心跳（8259 PIC + PIT ~100Hz → IRQ0；`[tick] N` 每秒）
- [x] COM1 串口输入（轮询 RX + 行编辑；与日志同口）
- [x] 内核态命令行：`help` / `mem` / `page` / `int`（另有 `uptime` / `halt`）
- [x] 基本中断统计（`int` 打印 IRQ0..15 非零计数 + timer ticks）
- [x] IDT 0..47：异常仍 panic；IRQ 可返回 + EOI；未处理 IRQ 仅计数后 EOI
- [x] `make smoke` 含 M0+M1+M2 标记；`make smoke-shell` TCP 喂命令

**验证**：`make smoke`（含 `[tick]` / `M2 shell ready`）；  
`make smoke-shell` 见 `help`/`mem`/`page`/`int`/`uptime` 输出。

---

## M3 — 用户态与进程 `[x]`

Goal 提示词（历史）：[`docs/GOAL_M3.md`](GOAL_M3.md)

- [x] GDT + TSS + Ring3（`iretq` 进入；`syscall`/`sysretq` 往返）
- [x] syscall：`write`(1) / `yield`(24) / `exit`(60) — Linux 号段，极小语义
- [x] 静态 ELF 加载（嵌入 `user/init.elf`、`user/task2.elf`）
- [x] 协作多任务（`yield` 切换；无抢占）
- [x] init + task2；全退出后回内核 idle/shell
- [x] `make smoke-user`；`SYSCALLS.md` 登记三调用

**验证**：`make smoke-user` 见 `Hello from Ring3`、交错 `yield`、`M3 userland OK`。

---

## M4 — VFS + 存储 `[x]`

Goal 提示词（历史）：[`docs/GOAL_M4.md`](GOAL_M4.md)

- [x] 块设备：PCI AHCI port0 `blk_read` / **`blk_write`**（MMIO map）
- [x] GPT → ESP → FAT16 挂载
- [x] VFS 根挂载 + `open`/`read`/`write`/`close`/`mkdir`；shell `ls`/`cat`
- [x] 每任务 FD；syscall `read`/`write`/`open`/`close`/`mkdir`
- [x] ESP 写入 `hello.txt` + `bin/*.elf`；从盘加载 init/task2
- [x] **FAT16 根写持久化** + 自检 `HelixFATWriteOK`（`/HELIXW.TXT`）
- [x] `make smoke-fs`

**验证**：`make smoke-fs` 见 `HelixFS OK`、**`HelixFATWriteOK`**、`FAT / RW`。

---

## M5 — Linux 兼容子集 `[x]`

Goal 提示词（历史）：[`docs/GOAL_M5.md`](GOAL_M5.md)

- [x] 扩展 syscall（uname/getdents64/brk/fstat/…）；未知 → `-ENOSYS`
- [x] 等价 applet 集 **helixbox**（echo/cat/ls/uname/sh -c/smoke）
- [x] `uname` 默认 **Helix**（诚实，非 Linux）
- [x] `third_party/README` + 可选 **`/bin/busybox`**（`echo HelixBusyBoxOK`）
- [x] `make smoke-linux`
- [x] `/tmp` ramfs 可写 + `mkdir`(83)
- [x] console 行缓冲 `[user] `

**验证**：`make smoke-linux`（BusyBox 和/或 helixbox 标记）。

---

## M6 — 动态链接与 musl `[x]`

Goal：[`docs/GOAL_M6.md`](GOAL_M6.md) · 完整 musl：[`docs/GOAL_M6_musl.md`](GOAL_M6_musl.md)

- [x] 最小：`PT_INTERP` + 自研 **`ld-helix`** + **`hello.dyn`** → `HelloDynOK`（`make smoke-dyn`）
- [x] 完整：**真 `ld-musl-x86_64.so.1`** + PIE **`hello.musl`**（NAS musl-gcc）→ **`HelloMuslDynOK`**（`make smoke-musl`）
- [x] PIE load bias（main @ `USER_BASE`，interp @ `0x50000000`）
- [x] `mmap`/`munmap`/`mprotect`/`arch_prctl` 等必要项
- [x] ESP：`/lib/ld-musl-x86_64.so.1`、`/lib/libc.so`（同镜像）、`/bin/hello.musl`

**验证**：`make smoke-dyn` · `make smoke-musl`。

## M8 — 用户态网络（UDP socket） `[x]`

Goal 模式提示词（可直接粘贴）：[`docs/GOAL_M8.md`](GOAL_M8.md)

- [x] `socket`/`bind`/`sendto`/`recvfrom`（UDP，AF_INET/SOCK_DGRAM）
- [x] `net_udp_output` + UDP RX demux → socket receive queue
- [x] `helixbox` applet：UDP 本地回环自检（sendto → recvfrom → `user_udp_ok`）
- [x] `getrandom` syscall（helixbox/BusyBox 兼容）
- [x] socket FD 通过 VFS 安装（`is_socket` 标志 + kmalloc wrapper）
- [x] TCP stub（`connect`/`accept`/`listen`/`sendmsg`/`recvmsg`/`setsockopt`/`getsockopt` → ENOSYS）
- [x] 宿主机 ↔ guest UDP ping（helixbox 向网关发 UDP → 超时 fallback）

**注意**：MSYS2 QEMU 的 `hostfwd=udp::` 不支持与已占用端口共存（无 SO_REUSEADDR），
`host_udp_timeout` 是预期结果；`user_udp_ok`（guest 内回环）为主验收。

**验证**：`make smoke-net`（含 `HelixNetOK` + **`user_udp_ok`**）。

## M7 — 网络与图形（可后置） `[x]`（网络主路径；图形见 M9）

Goal：[`docs/GOAL_M7.md`](GOAL_M7.md)

- [x] **e1000**（主验收路径）+ 可选 **virtio-net-pci** 后备；QEMU **user** net（`10.0.2.0/24`）
- [x] 以太网 + ARP + IPv4；**ICMP echo** 主验收（ping 网关 `10.0.2.2` → **`HelixNetOK`**）
- [x] socket syscall / 用户态 applet（helixbox UDP 回环 + fork 自检）
- [x] `make smoke-net`；framebuffer GUI **未做**（不挡 CLI）

**验证**：`make smoke-net` 串口含 `M7 net ready` · `10.0.2.15` · `ICMP echo reply` · **`HelixNetOK`**。

---

## M9 — 图形（GOP 帧缓冲） `[x]`

Goal（历史）：[`docs/GOAL_M9.md`](GOAL_M9.md)

- [x] `efi.h` 新增 `EFI_GRAPHICS_OUTPUT_PROTOCOL`（GOP）结构体
- [x] `boot_info.h` 新增 `fb_addr/fb_width/fb_height/fb_pitch/fb_bpp` 字段
- [x] `efi_main.c`：ExitBootServices 前 LocateProtocol 拿 GOP → 选 ≥640 宽模式 → 填 boot_info
- [x] `kernel/drv/fb.c`：`fb_init` / `fb_cls` / `fb_pixel` / `fb_rect` / `fb_put_char` / `fb_puts`
- [x] 内嵌8x16 VGA位图字体（ASCII 32–126）
- [x] `kernel_early_main`：`fb_init` 后绘制测试图案 → 串口 `[fb] fb_smoke_done`
- [x] `make smoke-fb`（独立，不挡 CLI/net smoke）

**注意**：MSYS2/ArchLinux/Ubuntu 打包的 OVMF（4MB 版本）不包含 QemuVideoDxe GOP 驱动，
`LocateProtocol` 会返回 `EFI_NOT_FOUND`。代码会 graceful fallback 到 headless 模式。
使用 QEMU 原生 OVMF（8MB+）或真机固件时 GOP 正常工作。

**验证**：`make smoke-fb` 串口含 `M9 framebuffer ready` + `fb_smoke_done`；QEMU 窗口可见蓝/红矩形 + 白色文字。

---

## M10 — fork/exec `[x]`

Goal（历史）：[`docs/GOAL_M10.md`](GOAL_M10.md)

- [x] `fork`(57)：复制 task struct + 内核栈 + 用户页表（子进程独立 PML4，用户页物理复制）
- [x] `execve`(59)：从 VFS 加载 ELF，替换当前 task 的用户空间、entry/stack/brk
- [x] `user_pages[]` 跟踪：每个 task 最多2048 页（8 MiB），fork 时自动追踪复制的页面
- [x] `vmm_copy_user_page_tables()`：4级页表递归复制，2MiB 大页拆分为4K 后逐页复制
- [x] helixbox smoke：fork 自检 → `ForkChildOK` + `ForkParentOK`
- [x] SYSCALLS.md / usys.h 更新

**验证**：`make smoke-linux` 串口含 `ForkChildOK` + `ForkParentOK` + `helixbox_smoke_done`。

---

## M11 — 进程生命周期（waitpid/pipe/shell） `[x]`

Goal（历史）：[`docs/GOAL_M11.md`](GOAL_M11.md)

- [x] `wait4`(61)：非阻塞轮询子进程，`WEXITSTATUS = (code & 0xFF) << 8`；zombie reap
- [x] `pipe`(22)：环形缓冲管道，`EAGAIN` 非阻塞 + 用户态 yield 轮询
- [x] FD 引用计数：`refcount` + `fd_hold`，fork/dup2 共享、exit/reap 递减
- [x] syscall 上下文切换修复：完整保存/恢复 caller-saved + callee-saved 寄存器（跨任务 resume）
- [x] **msh** 用户态 shell：`fork`/`execve`/`waitpid`/`pipe`/`dup2`；builtin echo/cat/ls/cd/help/exit；`|` 管道
- [x] `execve` argv 支持：从用户空间拷贝 argv，`setup_user_stack` 重建栈
- [x] helixbox 自检：`PipeOK` + `WaitOK`；msh smoke：`HelixMshOK`
- [x] `/bin/msh` 写入 ESP；`make smoke-linux` 含 msh pipeline 标记

**验证**：`make smoke-linux` 串口含 `PipeOK` + `WaitOK` + `HelixMshOK` + `helixbox_smoke_done`。

---

## M12 — cwd / chdir / 路径解析 / console stdin `[x]`

Goal（历史）：[`docs/GOAL_M12.md`](GOAL_M12.md)

- [x] per-task `cwd[256]`；`task_create` 默认 `"/"`；fork 经 memcpy 继承
- [x] `getcwd`(79) 返回真实 cwd；`chdir`(80) 解析路径并校验目录
- [x] `vfs_path_resolve`：相对路径 + `.`/`..`/`//` 归一化为绝对路径
- [x] `open`/`mkdir`/`execve`/`newfstatat` 经 cwd 解析
- [x] console stdin：`cons_read` 轮询 COM1，空则 `-EAGAIN`（用户态 yield 轮询）
- [x] msh：`cd`/`pwd` builtins；`ls` 默认 `"."`
- [x] helixbox 自检：chdir `/tmp/cwdtest` + 相对 open → **`HelixCwdOK`**

**验证**：`make smoke-linux` 串口含 `HelixCwdOK` + `HelixMshOK` + `helixbox_smoke_done`。

---

## M13 — 信号最小集 `[x]`

Goal：[`docs/GOAL_M13.md`](GOAL_M13.md)

- [x] per-task `sig_pending` / `sig_blocked` / `sighand[]`；`signal_task_init` / `signal_send` / `signal_deliver_current`
- [x] `kill`(62) / `rt_sigaction`(13) / `rt_sigprocmask`(14)
- [x] **SIGCHLD**：子 `exit` → 父 pending；默认忽略（与 wait 协作）
- [x] **SIGINT**：COM1 Ctrl+C（0x03）→ 当前用户 task；默认终止
- [x] **SIGTERM/SIGKILL**：default terminate；SIGKILL 不可屏蔽
- [x] helixbox：`fork` + `kill(child, SIGTERM)` + `wait4` → **`HelixSigOK`**
- [x] `SYSCALLS.md` / `smoke-linux` 检查 HelixSigOK

**验证**：`make smoke-linux` 串口含 `HelixSigOK` + `HelixCwdOK` + `HelixMshOK` + `helixbox_smoke_done`。

---

## 文档债务（随里程碑）

- [x] README / ARCHITECTURE / BUILD 对齐到 **M13**（2026-08-01）
- [x] `GOAL_M9`–`GOAL_M13` 历史 goal 补全；`GOAL_M4`/`M5` 修正过时表述
- [x] Logo：`img/Helix*.png` 引用进 README
- 每完成一阶段：更新本文件状态勾选与 `ARCHITECTURE.md` 中对应子系统
- M5 起 `SYSCALLS.md` 必须与代码同步

---

## M14 — TCP full stack `[x]`

Goal：[`docs/GOAL_M14.md`](GOAL_M14.md)

- [x] TCP 状态机：CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT → TIME_WAIT → CLOSED；LISTEN/SYN_RECEIVED active open
- [x] `socket`(SOCK_STREAM) + `connect`/`listen`/`accept`：is_socket=2 标记 + `helix_tcp_sock` 分配
- [x] TCP RX/TX 队列（每 socket 8 recv + 4 retransmission entries）
- [x] `sendto` / `recvfrom` 路由：UDP=socket1, TCP=socket2
- [x] 内核自测：ICMP gate ping 后 `tcp_init` → **`HelixTcpOK`**
- [x] `SYSCALLS.md` / `smoke-net` 更新；既有 UDP/ICMP 回归 OK

**注意**：TCP 连接需对端服务存在（QEMU user net 无 host 侧 TCP 服务），
`HelixTcpOK` 为内核层验收标记；用户态 TCP test 留待 M15 完善。

**验证**：`make smoke-net` 串口含 `HelixTcpOK` + `HelixNetOK` + `user_udp_ok`；M13 不回归。

---

## M15 — 用户态 TCP 完善 `[x]`

- [x] helixbox TCP echo 自检：`socket(SOCK_STREAM)` + `connect(10.0.2.2:8080)` + `sendto`/`recvfrom` → **`HelixTcpUserOK`**
- [x] `sendto`/`recvfrom` 路由：`is_socket==1` ↔ UDP；is_socket==2` ↔ TCP
- [ ] TCP 被动模式 hostfwd ↔ guest 端到端
- [ ] `sendmsg`/`recvmsg` 完善映射
- [ ] 更多 TCP 状态覆盖

**注意**：host echo server (`scripts/tcp_echo_server.py`) 仅在 `smoke-net` 期间启动。

**验收**：`make smoke-net` 串口含 `HelixTcpUserOK` + `HelixTcpOK` + `HelixNetOK` + `user_udp_ok`。

---

## M16 — TCP passive + sendmsg/recvmsg `[x]`

- [x] `sendmsg`(46) / `recvmsg`(47)：iovec coalesce/scatter + TCP/UDP 路由
- [x] TCP 被动 hostfwd：guest listen + accept → host curl 连入 → echo 回复

**验收**：`make smoke-net` 串口含 `HelixTcpPassiveOK`（被动 echo）+ M15 标记不回归。

---

## M17 — TCP retransmission (TXQ 驱动) `[x]`

Goal: [`docs/GOAL_M17.md`](GOAL_M17.md)

- [x] `helix_tcp_sock` 增加 `last_send_tick` 字段
- [x] `tcp_send_data()` 数据段入 TXQ 环形队列 + 更新 `snd_nxt`
- [x] `tcp_input()` ESTABLISHED ACK 清除已确认 TXQ 条目 + 推进 `snd_una`
- [x] `tcp_retransmit()` 新函数：1 秒超时重发，最大 3 次重试
- [x] `net_poll()` 调用 `tcp_retransmit()`
- [x] tcp.h 增加 TCP API 函数声明（消除 syscall.c implicit declaration 报错）
- [x] `libk/chkstk.S` 修复 Windows PE 链接 `__chkstk` 未定义符号
- [x] `tcp_init()` 保留已绑定 socket（不清理 active listener）
- [x] `fd_close()` 修复：TCP socket 调用 `tcp_free()` 而非 UDP 专用 `net_sock_free()`
- [x] `vfs_file` 增加 `flags` 字段 + `sys_fcntl` F_SETFL 实现（O_NONBLOCK 支持）
- [x] TCP 被动 pending 队列：SYN 到达但 listener 未创建时暂存 child；listener 建立后自动 adopt
- [x] Pending child 缓冲 ACK/data：listener 创建前到达的包暂存，创建后一次性注入 rxq
- [x] helixbox TCP passive fork 子进程 + 更长 accept 循环

**验收**：`make smoke-net` 串口含 `HelixTcpOK` + `user_udp_ok`；`fd_close` TCP socket 不再泄漏内存；pending SYN 队列正确工作。

---

## M18 — fb user-space interface + PS/2 keyboard `[x]`

Goal: [`docs/GOAL_M18.md`](GOAL_M18.md)

- [x] `kernel/drv/ps2.c`：PS/2 键盘 IRQ1 handler + scancode ring buffer + set-1 ASCII translation
- [x] `irq.c`：IRQ1 分发 → `ps2_handler()`；`irq_init` 调用 `ps2_init()` unmask IRQ1
- [x] `kernel/drv/fb.c`：`fb_get_info()` + `fb_map_user()` — 用户态 mmap 帧缓冲
- [x] `paging.h`：PTE_P/W/U 标志提升到头文件（供 fb.c 使用）
- [x] `sys_mmap` 扩展：`fd == -4` 映射 GOP 帧缓冲物理页到用户 VA
- [x] `sys_fb_info`（nr=546）：返回 width/height/pitch/bpp/size 到用户 struct
- [x] `sys_readkey`（nr=547）：非阻塞 PS/2 键盘读取
- [x] helixbox 自检：`HelixFBInfoOK` + `HelixFBMmapOK` + `HelixKbOK`

**验证**：`make smoke` 串口含 `HelixFBInfoOK` + `HelixFBMmapOK` + `HelixKbOK`；
`make smoke-fb` QEMU 窗口可见蓝色矩形（mmap 写入）；启动日志含 `[ps2] keyboard ready`。

---

## M19 — TUI shell (fb + PS/2 keyboard) `[~]`

Goal：基于 M18 提供的 fb mmap + PS/2 键盘，编写用户态 TUI shell。
运行模式：在 framebuffer 上绘制文本 UI + 输入栏，PS/2 键盘输入命令。

- [x] 用户态 mini-terminal：`tui_init()` / `tui_putc()` / `tui_puts()`，写入 GOP mmap 区域
- [x] 内嵌 8x16 VGA 位图字体（95 ASCII 字符，MSB-first）
- [x] PS/2 键盘扫描码 → ASCII 翻译（系统调用 sys_readkey 完成；backspace/Ctrl+D/Ctrl+C）
- [x] 内置命令：`help` / `clear` / `echo` / `ls` / `cat` / `ps` / `tcpstat` / `time` / `reboot` / `exit`
- [x] 串口 + fb 双输出（fb 渲染，串口备份 log）
- [x] `bin/tui` 写入 ESP；内核 shell `tui` 命令通过 `task_exec_path` 启动
- [x] 文本网格（80x30 @ 1024x768），`scroll_up()` 滚屏，行编辑
- [x] 三重故障重启（`xor %rax,%rax; mov %rax,%cr3`）

**注意**：MSYS2/ArchLinux/Ubuntu 打包的 4MB OVMF 不含 QemuVideoDxe，本地 QEMU 测试时
`fb_init` 返回 -1，`tui` 优雅退出 `[tui] no framebuffer`。在原生 OVMF（8MB+）或真机
固件上 TUI 可正常工作。

**验证**：`make esp` 串口含 `+ bin/tui`；`make` 输出 `build/user/tui.elf`（11 KiB）；
内核 shell 输入 `tui` 命令后 `task_exec_path(tui) -> 0x...`（任务已创建）。
完整 fb 渲染验证需带 QemuVideoDxe 的 OVMF 环境。

---

## M21 — FAT32 完善 `[x]`

Goal：[`docs/GOAL_M21.md`](GOAL_M21.md)

- [x] `fat_free_chain()` 释放 cluster chain（FAT entries → 0）
- [x] `root_unlink_and_free()` 同时支持 FAT16 固定 root + FAT32 root cluster chain
- [x] `fat_selftest_write` 改走新 helper，FAT32 二次启动 stale-cleanup 修复
- [x] `mkdisk.py` 新增 `build_fat32_volume()`；ESP ≥ 64 MiB 自动选 FAT32
- [x] FAT32 真实大盘镜像挂载（mformat + mcopy + mkdisk.py GPT 包装）→ `selftest OK (FAT32): HelixFATWriteOK`
- [x] FAT32 二次启动 cleanup 验证：HELIXW.TXT cluster=3 size=16，新分配，旧 chain 已 free

**验收**：
- FAT16：`make smoke-fs` 不回归（含 `mounted FAT16` + `HelixFATWriteOK`）
- FAT32：64 MiB ESP 镜像两次连 boot，selftest 两次 OK；HELIXW.TXT cluster 两次都是 3（重新分配证明 cleanup 走通）

**已知限制**：`build_fat32_volume` 的 nested dir materialization 有 bug（顶层文件 OK，子目录递归会丢 dirent）。
本里程碑**只**用 flat root + mtools 注入子目录；自动化 FAT32 大盘脚本留后续。

---

## M22 — 抢占式调度（preemptive）`[x]`

Goal：[`docs/GOAL_M22.md`](GOAL_M22.md)

- [x] `kernel/arch/x86_64/timer.c`：`timer_on_irq` 每 tick 累加 `g_preempt_pending`
- [x] `timer_preempt_pending()`：`__atomic_exchange_n` 读并清零 + `PREEMPT_THRESHOLD=8` 阈值
- [x] `kernel/proc/syscall.c:1311` 与 :1313 之间插入抢占检查
  - gate 1：`task_count_alive() > 1`（单任务路径无开销）
  - gate 2：`timer_preempt_pending() >= PREEMPT_THRESHOLD`（≈80ms 节流）
  - 命中后 `task_yield()`；不调 `net_poll()`（sys_yield 已调）
- [x] helixbox 加 `HelixPreemptOK`：fork 心跳子进程写 20 个 `.`，主 task yield 30 次
- [x] `make smoke-fs` 不回归；`make smoke-net` 含 HelixPreemptOK + HelixTcpUserOK + HelixTcpPassiveOK

**注意**：抢占**不**改 syscall 入口寄存器窗口；`task->regs` 仍由 syscall.c:1313-1335 拥有。
阈值 8 让抢占触发频率 ≈12 次/秒，对心跳推进够用，对 `cmd_tcp_smoke` 1000×20
yield 内层循环开销可接受。

**验证**：`make smoke-linux` 串口含 `[helixbox] HelixPreemptOK`（child 写 20 dots 后退出；
parent 继续打印 HelixPreemptOK）；`make smoke-net` 含 HelixPreemptOK + TCP user/passive 不回归。

---

## M23 — PS/2 鼠标支持 `[x]`

Goal：PS/2 控制器第二通道（IRQ12）启用 + 鼠标 3-byte packet 解析；用户态可读鼠标事件。

- [x] `kernel/drv/ps2.c` 抽 `ps2_write_cmd()` / `ps2_write_aux()` / `ps2_flush_data()` 公共 helper
- [x] `ps2_init()` 启用 aux port：`0xA8` + `0x60 | 0x47` + `0xD4 0xF4` + `0xD4 0xF3 0x64`
- [x] `ps2_mouse_handler()` 累积 3-byte packet → `{dx, dy, buttons}` struct → ring buffer (64)
- [x] `kernel/arch/x86_64/irq.c`：IRQ12 → `ps2_mouse_handler()`，`pic_unmask(12)`
- [x] `sys_mouse_read`(548)：从 ring buffer 读事件到用户 struct；空时返回 `-EAGAIN` (-11)
- [x] helixbox 自检：sys_mouse_read 路径探针 → `HelixMouseOK`（有事件追加 "(events)"）
- [x] y 轴翻转：PS/2 y+ = 屏上 → 我们 dy>0 = 屏下（GUI 约定）
- [x] overflow flag (bit 6/7) 丢弃，避免 dy/dx wrap 误判

**验收**：`make smoke-linux` 串口含 `[ps2] mouse ready (IRQ12 unmasked)` + `[user] HelixMouseOK`；
`make smoke-fs` EXIT=0（FAT 写不回归）；smoke-net 仍因 host echo server 未启 baseline 失败
（pre-existing M21 issue，非 M22/M23 回归）。

---

## M24 — POSIX file ops 收尾 `[x]`

Goal：补全 Linux ABI 差距（poll / fsync / unlink / rmdir / rename）+ silent ENOSYS。

- [x] `include/helix/vfs.h` 加 `struct helix_pollfd` + POLL defines + `vfs_ops` 5 新字段（poll/unlink/rmdir/rename/fsync）= 14 字段 112 字节
- [x] `kernel/proc/syscall.c`：`sys_poll` / `sys_ppoll`（不阻塞 + `vfs_poll_one`）+ `sys_unlink` / `sys_rmdir` / `sys_rename`（resolve_user_path + vfs_*）+ `sys_fsync` / `sys_fdatasync`
- [x] dispatch table 加 `case 7 / 271 / 74 / 75 / 82 / 84 / 87`，default 改 silent `ERR(ENOSYS)`（不 kprintf）
- [x] `kernel/fs/vfs.c`：`vfs_unlink/rmdir/rename` 分发到 FAT + ramfs ops；`vfs_poll_one` default (stdin → POLLIN, stdout → POLLOUT, regular → POLLIN|POLLOUT)
- [x] `kernel/fs/fat.c`：`fat_resolve_parent` + `dir_unlink_at`（mark 0xE5 + `fat_free_chain`）+ `dir_rename_at` + `dir_is_empty` + `fat_unlink_path/rmdir_path/rename_path`
- [x] `kernel/fs/ramfs.c`：`node_release` + `node_child_count` + `ramfs_unlink_op/rmdir_op/rename_op`，wire 到 `g_ramfs_ops`
- [x] `user/helixbox.c`：`HelixPollOK`（poll fd=0） + `HelixUnlinkOK`（create + unlink + rmdir + rename） + `HelixFsyncOK`（fsync + fdatasync + bad-fd EBADF + O_TRUNC 重置 size 0）
- [x] **M24.1 cross-dir rename**：`kernel/fs/fat.c` `dir_rename_cross`（新父目录写新 dirent 复用同一 cluster + 保留时间戳 + `fat_update_dotdot` 更新 `..` + `dir_unlink_at(old, 0)` 不 free chain）+ `fat_dir_has_ancestor`（走 `..` 链拒绝移入自身子树）+ `fat_rename_path` cross-dir 分发；`kernel/fs/ramfs.c` `ramfs_rename_op` 去掉同目录限制改 reparent + 祖先环检测；`dir_unlink_at` 加 `free_chain` 参数（rename 复用时不释放）
- [x] init message: `"init syscall dispatch (M24 poll/ppoll + silent ENOSYS)"`

**验收**：`make smoke-linux` 串口含 `HelixPollOK` + `HelixUnlinkOK` + `HelixFsyncOK`，无 FAIL；
`grep -E "\[syscall\] ENOSYS" serial.log` 应为空（silent）；smoke-fs EXIT=0 不回归。
`HelixRenameOK`（ramfs /tmp/rd→/tmp/rf.txt + FAT /etc→root→move-back 全链路）。

**已知边界**: rename 同目录 + cross-dir 均支持（FAT 跨目录移动目录时 `..` 已更新；不 free
cluster 链保证原内容完整）；fsync 是 no-op (FAT+ramfs synchronous write 保留 op 字段为未来 buffered fs 留口)。

---

## 路线 D — 收尾修复补全 `[x]`

路线 A→B→C 完成后进入 "已知缺陷修复 + 验证闭环 + 用户体验提升" 阶段。

### D1 — smoke-net 闭环 `[x]`

Goal：把 `make smoke-net` 从"知道失败"变成"知道通过"，让 HelixTcpUserOK / HelixTcpPassiveOK 真
正作为验收 marker（不再 soft-warn）。

- [x] `Makefile` smoke-net 目标：启动 `tcp_echo_server.py` 后**轮询 8080 端口**直到 bind
  （替代 racey `sleep 8`），server stderr 重定向到 `tcp_echo_server.log` 方便排错
- [x] `Makefile`：`HelixTcpUserOK` / `HelixTcpPassiveOK` 从 soft-warn（`echo ... WARN`）
  升为 hard-fail（`exit 1` + 打印 serial.log）
- [x] `kernel/net/tcp.c`：`[tcp] SYN retransmit: max retries reached` log 节流到每 30s 一次
  （`static u64 s_last_retransmit_log + timer_ticks()`），serial.log 不再被刷屏淹没
  让 grep 可信

**验收**：`make smoke-net` EXIT=0 + serial.log 含 `HelixNetOK` + `HelixTcpOK` +
`HelixTcpUserOK` + `HelixTcpPassiveOK`（真从 guest→host echo 收到回复）。

### D2 — `build_fat32_volume` nested dir 修复 `[x]`

Goal：64 MiB FAT32 镜像能正确生成 `EFI/BOOT/BOOTX64.EFI` 嵌套结构，OVMF 能找到 bootloader
路径（M21 自标"已知限制"）。

- [x] `scripts/mkdisk.py`：`materialize_dir` 加 `is_root=False` flag；root 路径不加 `.`/`..`
  entries（FAT spec 禁止 root 有 dot entries）
- [x] `scripts/mkdisk.py`：整段 root children loop（line 304-359，56 行）替换为单行
  `materialize_dir(tree, 0, root_cl, is_root=True)`；递归子目录调用加 `is_root=False`
- [x] 之前 bug：`kind=="dir"` 分支分配 fresh `new_root` cluster 写 dirent，但原始
  `root_cl` 永远是全 0，dir dirent 跑到 orphan cluster；OVMF 找不到 `EFI/BOOT/...`

**验收**：mtools `mdir -/ -i test_fat32_raw.img ::EFI/BOOT` 列出 `BOOTX64.EFI`；
`make smoke-fs` EXIT=0（FAT16 路径不回归）。

### D3 — msh 行规程增强 `[x]`

Goal：msh 跟 Linux bash 一样有 cursor 行编辑 + history + Ctrl+A/E/W/U，箭头键可工作。

- [x] `kernel/drv/ps2.c`：`ps2_handler` 加 `0xE0` prefix 状态机（`g_e0_pending` flag）
  — 之前 bit 7 set 被 `sc & 0x80` 滤掉，0xE0 直接被丢；现在接 0xE0 后等下一个 byte，
  箭头键 make（0x48/0x50/0x4B/0x4D）→ 输出 ESC [ A/B/C/D（xterm 标准序列）
- [x] `user/msh.c`：`msh_readline` 重写为 cursor 模型
  - `cur` 独立于 `len`，任意位置可插入/删除
  - ESC 状态机（ST_IDLE → ST_ESC → ST_CSI）解析箭头键
  - Ctrl+A 行首 / Ctrl+E 行尾 / Ctrl+W 删词到空白 / Ctrl+U 清行 / Ctrl+C 输出 ^C
  - 16 条 history ring（`msh_history[16][256]`）；第一次 Up 时把当前行存到 `msh_draft`，
    再 Up 翻老历史，Down 回到 draft
  - `msh_redraw`：`\r` + 整行 + 尾部空格擦除 + `(len-cur)` 个 `\b`
- [x] sys_read 不变路径（`g_cons_ops.read` 已 wired 到 `cons_read` → ps2 轮询）；无新 syscall

**验收**：`make` EXIT=0（kernel + user 编译干净）；`make smoke-linux` EXIT=0（HelixLinuxOK +
HelixPreemptOK + cwd + sig 不回归）；`make smoke-fs` EXIT=0（FAT 不回归）。
手工验证：msh 输入 `echo hello<Up>` 自动补全；Ctrl+A 跳行首；Ctrl+W 删 word。

### D4 — 内存安全 trio (munmap / mprotect / vmm_unmap) `[x]`

Goal：实修 Sakura AI 扫描 ([issue #1](https://github.com/BB0813/HelixOS/issues/1))
CRITICAL #1/#2/#3：现在 `sys_munmap` / `sys_mprotect` / `vmm_unmap_user_range`
都是空 stub，用户 munmap 后页面仍占用 (leak + busybox mmap loop 失控),
mprotect 返回成功但实际不变（破坏 W^X 语义 + 任何依赖 PROT_NONE 的应用 silent fail）。

- [x] `kernel/mm/vmm.c` `vmm_unmap_user_range(virt, len)` — 真实：逐页 `paging_unmap_4k`（D4.2 落地 per-task PML4 后启用）
- [x] `kernel/mm/vmm.c` 新增 `vmm_set_prot(virt, len, prot)` — 真实：`paging_set_prot_range` toggle PTE_W
- [x] `kernel/proc/syscall.c` `sys_munmap` / `sys_mprotect` — 真实：校验对齐 + 范围窗口后调用 vmm 实现
- [x] `kernel/arch/x86_64/paging.c`：`paging_unmap_4k` + `paging_set_prot_range` + `table_count_present` 已实现
- [x] `kernel/proc/task.c` task_exit 注释文档化共享 PML4 限制；user_pages[] 清理
- [x] helixbox smoke：HelixMunmapOK + HelixMprotectOK markers 验证 mmap 路径
- [x] **D4.2 (M25)**：per-task PML4 激活 + 真实 unmap/prot — 见 D4.2 节

**验收**：`make smoke-linux` 全 marker pass（含真实 HelixMunmapOK + HelixMprotectOK，
HelixPreemptOK 验证 CR3 切换）; `make smoke-fs` EXIT=0; `make smoke-net` EXIT=0。

**已知限制**：COW 延后（fork 仍 eager-copy，refcount 已就绪）；PROT_NONE 保持
present+readable（无 fault-recovery handler，避免 #PF panic）；`make smoke-shell`
受 shell 仅在 idle loop 处理命令的时序限制（pre-existing，与 D4.2 无关）。

### D4.2 — per-task PML4 激活 + 真实 unmap/mprotect (M25) `[x]`

Goal：解除共享单 PML4 硬阻塞，落地真实 unmap/prot，修复 issue #1 CRITICAL
#1/#2/#3（此前 sys_munmap/sys_mprotect/vmm_unmap_user_range 均为 no-op stub）。

- [x] `kernel/mm/pmm.c` per-page refcount：`pmm_page_own/share/deref/refcount`，`paging_unmap_4k` 改 `pmm_page_deref`（共享页不被 peer 释放）
- [x] `kernel/arch/x86_64/paging.c`：`g_kernel_pml4`（boot identity 模板，永不持有 user 页）+ `paging_set_pml4`（换 CR3 + TLB flush）
- [x] `kernel/mm/vmm.c`：`vmm_clone_kernel_pml4`（克隆模板，kernel leaves 共享）+ `vmm_destroy_address_space`（递归释放 per-task tables + user phys）+ 真实 unmap/prot
- [x] `kernel/proc/task.c`：`task_create` 分配 per-task pml4；`task_activate` 在每次 context switch 加载 CR3；fork 存储子进程 pml4 副本；exit 先切到安全 pml4 再 destroy 旧地址空间
- [x] `kernel/proc/exec.c` + `kernel/proc/elf.c`：ELF + stack 直接加载进 task 自己的 pml4；`elf_load_bitmaps_reset` 防跨地址空间 dedup 污染
- [x] `kernel/proc/syscall.c`：`sys_execve` 换新地址空间加载后 destroy 旧空间
- [x] `user/helixbox.c`：HelixMunmapOK 增强 — munmap 后 MAP_FIXED 同址重 mmap，验证 PTE 真清 + pages 释放
- [x] **fork U-bit bug 修复**：`vmm_copy_user_page_tables` 顶层 PML4 写丢失 PTE_U
      （err=0x15 = P=1+U=1+IF=1 → 子进程首条指令取指 #PF）→ 继承 parent 的 W|U|PWT|PCD

**验收**：`make` clean; `make smoke` / `smoke-linux` / `smoke-fs` / `smoke-net` 全 pass;
`make smoke-shell` pre-existing 时序失败（shell 只在 idle loop 处理命令，userland 链跑完前不会响应）。

**已知限制**：fork 仍 eager-copy（COW 延后 M26+）；PROT_NONE 不丢 P 位。

### D5 — getrandom 真熵 + heap full coalesce + execve argv 修复 `[x]`

Goal：剩余 CRITICAL #4 (getrandom deterministic) + 部分 MAJOR（heap 外碎片 /
execve argv leak）。这一批不阻塞功能但影响可信度。

- [x] `kernel/proc/syscall.c` `sys_getrandom`：检测 CPUID `RDRAND` (leaf 1 ECX bit 30)
      走硬件 RDRAND（10 次重试）；不可用时 fallback 到 Galois LFSR (`lfsr_next_byte`)
      over TSC + 堆地址。明确注释：headless QEMU 无 RDRAND 仍 deterministic-ish；
      helixbox 加 `HelixGetrandomOK` (verify buffer 非全 0 + non-pattern)
- [x] `kernel/mm/heap.c` `kfree` full coalesce：双向 — 线性 free-list scan 找到
      next_phys 并吸收，再找 prev_phys (其 end == freed block start) 吸收 freed
      block，合并后写新 size 到结果块头。验证：连续 kfree 三个相邻块 → 再 kmalloc
      大块应一次成功 (HelixMallocOK smoke)
- [x] `kernel/proc/syscall.c` `sys_execve` argv：**验证无 leak** — argv 被 push 到
      user stack 而非 kernel heap，失败路径无 kernel 侧副本需 kfree（D4 审查确认）
- [ ] （可选，延后） `kernel/net/tcp.c` `txq[4] → txq[16]`（MAJOR #2）；当前
      helixbox 跑大文件 cat 够用，无 smoke 阻塞

**验收**：helixbox `HelixGetrandomOK` + `HelixMallocOK`；`make smoke-linux`
EXIT=0；`make smoke-fs` EXIT=0。

**已知边界**：RDRAND 在老 QEMU (<6.x) + TCG 可能 disabled；fallback LFSR 在
单任务 boot 早期仍欠熵，但比纯 `timer_ticks + i*37` 强。

### D6 — UI/UX 清理 + 文档 `[x]`

- [ ] （MINOR，延后） `[fat]` / `[net]` / `[tcp]` kprintf 加 ANSI color prefix，
      headless 下用 `isatty(serial)` 判断（永远 false → 不变色不破坏 log）
- [x] （MINOR） 集中地址窗口常量到 `include/helix/mm_layout.h`（USER_BASE、
      USER_STACK_TOP、USER_LOW window、ld-helix 0x50000000 等），删 syscall.h
      内的 inline 定义
- [x] （MINOR） syscall.c 每个入口一次性 `fd_init_task_stdio()`；当前每个
      handler 重复调，改为 `syscall_entry_c` 入口处一次
- [x] GOAL_D4.md / GOAL_D5.md / GOAL_D6.md 创建
- [x] ROADMAP / ARCHITECTURE / SYSCALLS / README 加 D4–D6 节

### D7 — DX 痛点 + FAT stat 真实化 `[x]`

### D7.1 — mkesp.sh 对必需 user 二进制硬失败 `[x]`

Goal：修调试陷阱 — `git clean -fd build/` 删掉 helixbox.elf 后 `make -j2` 不重建
user binaries，直接 `bash scripts/run-qemu.sh` 绕过 `make esp` 的 user 依赖 →
ESP 缺 helixbox → busybox chain 正常跑但 helixbox exec 静默失败 → 假 "musl hang"
（D4 调试浪费 30 分钟根因）。

- [x] `scripts/mkesp.sh`：init.elf/task2.elf/helixbox.elf/msh.elf/tui.elf 缺失时
      `exit 1` + 明确错误 `"run 'make user' first"`（替代之前的静默跳过）

**验收**：`rm build/user/*.elf && bash scripts/mkesp.sh` → 非零退出 + 明确报错。

### D7.2 — FAT stat 真实字段 + CMOS RTC `[x]`

Goal：`fstat` 不再返回假 0/1970-01-01 时间戳 — FAT 真实 mtime/atime/ctime + st_ino。

- [x] `kernel/arch/x86_64/timer.c`：`rtc_unix_seconds()` — CMOS 0x70/0x71 读
      sec/min/hour/day/mon/year，status A bit 7 等 update-in-progress，BCD→bin，
      Hinnant civil→unix 换算（year=2000+）
- [x] `kernel/fs/fat.c`：
  - `struct fat_file` 加 wrt/acc/crt time/date 5 字段
  - `struct fat_dirent_meta`（clus/size/attr + 4 组 date/time）
  - `find_in_dir()` 读 dirent offset 14/16/18/22/24（FAT u16 date/time 布局）
  - `fat_resolve()` 加 `out_meta`；3 个 caller 更新
  - `fat_date_to_unix()` / `fat_unix_to_date()`（Hinnant inverse）
  - `fill_83_dirent()` 用 RTC 真实时间 stamp 新建文件
  - `fat_fstat()` 填 `st_ino=start_clus` + `st_mtime/atime/ctime`
- [x] `user/helixbox.c`：`HelixStatOK` smoke — open `/HELIXW.TXT`，fstat，
      验证 `st_size==16 && st_ino!=0 && st_mtime!=0`

**验收**：`make smoke-linux` 含 `HelixStatOK`，无 FAIL；`ls -l` 显示真实时间戳
（非 1970-01-01）。

---

## M20 — VFS ext + 用户态补全 `[x]`

Goal：路线 A→B→C→D 完成后收尾 4 个真实差距 — FAT 子目录遍历完整、permission
syscall 显式 ENOSYS、深嵌套子目录验证、BusyBox 多 applet 真实 smoke、msh 增强。

- [x] **FAT subdir 完整** — `fat_getdents64` 改用 `fs_priv` 存的 `fat_dir_iter`
  （clus/sec/off 状态），subdir open 走 cluster chain walk；FAT16 root region
  保留 fixed-region 路径（root 不是 cluster chain）。`fat_resolve` 加 `out_attr`
  报告 leaf 是 dir；`fat_open` 检测 `attr & 0x10` 时返回 `is_dir=1` +
  `fs_priv=fat_dir_iter`，绕开 `fat_file` 分配
- [x] **chmod/chown/utimes 显式 ENOSYS** — `syscall.c` 在 `case 59 execve` 之后加
  `case 21/90/91/92/93/94/132/133/280` 都 `ret = ERR(ENOSYS); break;`（**不**走 default
  kprintf 刷屏）。BusyBox `chmod`/`chown`/`touch` 收到 ENOSYS 走标准错误路径
- [x] **FAT 深嵌套子目录验证** — 新 `scripts/mkdisk_deep.sh`：
  - 4 级目录 `a/b/c/d/file.txt` (含 `HELIX_DEEP_OK\n`)
  - `mkdisk.py` 加 `--add-tree HOSTDIR::` 递归遍历
  - `mkdisk.py` 加 `--raw-fat` 输出 raw FAT volume（无 GPT 包装），mtools 可直接读
  - mtools 验证：`mdir -/ -i out/helix-deep.raw.img ::a/b/c/d` 列出 file.txt；
    `mtype -i out/helix-deep.raw.img ::a/b/c/d/file.txt` 输出 `HELIX_DEEP_OK`
- [x] **etc 资产** — `esp_assets/passwd` (root entry) + `esp_assets/welcome.txt`
  (`HELIX_WELCOME_OK\n`) stage 到 ESP `/etc/`；`mkesp.sh` 加 `--add` 两行
- [x] **helixbox subdir probe** — `cmd_smoke` 加 `ls /etc` + `cat /etc/passwd` +
  `cat /etc/welcome.txt` + `ls /lib` 4 个探针，验证 kernel subdir getdents64 + open
- [x] **BusyBox 多 applet 真实 smoke** — `linux_compat_run_busybox_applets` 在
  `exec.c` 用 `g_bb_applets[][16]` 5-applet 表 + 模块级 `g_bb_idx` 状态。
  每个 applet exit → `task_set_exit_all_hook(linux_compat_run_busybox_applets)`
  自递归 → next applet。Applet 链：`echo HelixBusyBoxOK` → `cat /etc/welcome.txt`
  → `echo BB2_OK` → `true` → `echo HELIX_BB_DONE`。最后 → `msh_compat_run_smoke`
- [x] **msh 增强 (6 builtin)** — `user/msh.c` 加 `bi_alias` / `bi_unalias` /
  `bi_export` / `bi_unset` / `bi_test` (含 `[` 形式) / `bi_type`。文件静态
  `msh_aliases[16]` + `msh_envtab[32]` 表。**`msh_exec_line` 重构**：拆为
  `msh_exec_line` + `msh_exec_pipeline` + `msh_strtok_r`；支持 `;` statement
  separator。**alias expansion** 在 `msh_exec_pipeline` 早段：lookup alias body
  → 拼接 `body` + space + tail args → 重 tokenize → 替换 argv0
- [x] **msh bi_test fix** — `-f FILE` 单参操作符优先于 binary `=` 检查
  (避免 "test: need binary expr" 误报)。`/hello.txt` 存在 → 退出码 0
- [x] **kernel heap bump** — `heap.c` HEAP_PAGES 1024 → 2048 (4 → 8 MiB)，
  容纳 3+ BusyBox ELF 重 load (each ≈ 1.1 MiB)

**验收**：
- `make smoke-fs` EXIT=0（FAT16 不回归）
- `make smoke-linux` 串口含 `root:x:0:0:root:/root:/bin/sh` (cat /etc/passwd) +
  `HELIX_WELCOME_OK` (cat /etc/welcome.txt) + `BusyBox chain done (5 applets)`
  + `HELIX_MSH_ALIAS_OK` (alias) + `HELIX_MSH_EXPORT_OK` (export) +
  `HELIX_MSH_TEST_OK` (test -f) + `HELIX_MSH_DONE` (pipe + cat)
- `make scripts/mkdisk_deep.sh` mtools 验证 4 级 subdir OK
- kernel log 不再有 `[syscall] ENOSYS nr=90/91/92/...` 刷屏

---

## M19 — TUI shell (fb + PS/2 keyboard) `[~]`


---