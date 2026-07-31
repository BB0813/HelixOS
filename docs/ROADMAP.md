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

## M15 — 用户态 TCP 完善（后续候选）`[ ]`

- [ ] helixbox `HelixTcpOK` 用户态 TCP echo/ping 自检（若环境允许）
- [ ] TCP 被动模式 hostfwd ↔ guest 端到端
- [ ] `sendmsg`/`recvmsg` 完善映射
- [ ] 更多 TCP 状态覆盖


