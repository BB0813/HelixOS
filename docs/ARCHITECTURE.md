# HelixOS Architecture

## Naming

| 名称 | 含义 |
|------|------|
| **HelixOS** | 整个操作系统项目（内核 + 兼容层 + 文档 + 工具） |
| **Helix** | 自研内核本身 |
| Linux 兼容层 | 内核内的 syscall / 行为映射，**不是** Linux 内核 |

宣传与文档必须诚实区分：自研内核 / Linux 兼容 / 第三方用户态。

## Layering

```text
自研 App / 调试工具
        ↓ 自研最小 API（可选，服务调试）
Linux 用户态（musl, BusyBox, msh, helixbox, …）
        ↓ Linux syscall 兼容层（主路径）
Helix 内核（调度 · 内存 · VFS · 驱动 · 网络 · 进程）
        ↓
x86_64 + UEFI + 硬件
```

### 原则

1. **一套内核语义**，尽量 POSIX 味，方便映射 Linux ABI。
2. Linux 兼容是 **syscall 号 + 必要的 loader 行为**，不是重新实现 Linux。
3. 自研 API 保持极小，默认不与 Linux 抢第一公民地位。
4. 驱动优先 QEMU 友好：串口、GOP/framebuffer、AHCI、e1000/virtio-net。

## Boot path (M0 → M13)

```text
UEFI firmware (OVMF)
    → GPT disk / ESP FAT / BOOTX64.EFI
         boot → (optional GOP) → ExitBootServices → kernel_early_main
         M1: pmm / identity / gdt / idt / heap
         M2: PIC+PIT / kernel shell
         M9: fb_init → test pattern（无 GOP 则 headless）
         M4: AHCI → GPT → FAT16 RW 挂载 → VFS /
         M7: e1000 + ARP/IPv4/ICMP 自测 → HelixNetOK
         用户态链式 smoke（exit-all hook）：
           M6  ld-helix hello.dyn → musl hello.musl
           M5  BusyBox echo 和/或 helixbox smoke
                 · UDP 回环 user_udp_ok
                 · fork/exec ForkChildOK
                 · pipe/wait PipeOK WaitOK
                 · cwd/chdir HelixCwdOK
                 · signals HelixSigOK（kill SIGTERM）
                 · TCP echo HelixTcpUserOK（hostfwd → host echo server，M15）
                 · TCP passive HelixTcpPassiveOK（guest listen + host connect，M16）
           M11 msh -c "echo HelixMshOK | cat"
         全退出后 → 内核 idle / shell
```

**内核仍是单一 EFI 映像**；早期测试 ELF 可嵌入（fallback），主路径从 ESP 加载。

### M4 storage

| 项 | 选择 |
|----|------|
| 块设备 | PCI class 01:06 AHCI，port0，同步 READ/WRITE DMA EXT |
| MMIO | `paging_map_mmio`（ABAR 常在 identity 外） |
| 分区 | GPT，ESP type GUID |
| FS | `/` → **FAT16/FAT32 可写**（根目录 create/write/mkdir；写回 ESP 镜像）；`/tmp` → **ramfs 可写** |
| VFS | 路径分流：`/tmp…`→ramfs，其余→FAT；相对路径经 per-task cwd 解析（M12） |
| FD | 每任务 16 槽；0/1/2 = 串口 console（静态，不 free）；`refcount` + `fd_hold`（M11） |
| 限制 | FAT32 写路径未做；FAT 写主要覆盖根 8.3 |

### M3 / M10–M12 process model

| 项 | 选择 |
|----|------|
| 地址空间 | **每任务独立 CR3/PML4**（M10）：kernel 映射共享，用户页独立物理复制 |
| 进入用户 | `iretq`；`syscall` / `sysretq` |
| 调度 | **协作** yield/exit + **M22 IRQ0 每 tick 抢占点**（阈值 8 ticks ≈80ms，syscall 返回路径检查 + `task_yield()`；不调 `net_poll` 避免双倍开销） |
| 阻塞 I/O | 必须 **EAGAIN** + 用户态 `yield` 轮询（pipe / wait4 / console stdin） |
| 输出 | `write` → console 行缓冲 → `[user] ` + 串口 |
| 进程 | `fork`(57) 独立 PML4 + 页表递归复制；`execve`(59) 替换用户空间 + argv |
| 生命周期 | `wait4`(61) 非阻塞 + zombie reap；`pipe`(22)；`dup2`(33)；exit 关继承 FD |
| cwd | per-task `cwd[256]`；`getcwd`/`chdir`；`vfs_path_resolve` 归一化 |

### M1 memory notes

- `phys_ceiling` 只统计 RAM-like 类型，**不含** MMIO/Reserved。
- PMM 只把 `EfiConventionalMemory` 标为空闲。
- Identity map：`[0, phys_ceiling)` 2MiB pages。

### M2 interrupts & shell

| 项 | 实现 |
|----|------|
| PIC | 8259 重映射 master=`0x20` slave=`0x28` |
| 时钟 | PIT ch0 mode3 ≈100Hz → IRQ0 |
| 心跳 | 主循环 `timer_poll_heartbeat` 打印 `[tick] N`（**不在 IRQ 里 kprintf**） |
| IDT | 0..31 异常 → panic；32..47 → `irq_handle` + EOI |
| 输入 | COM1 轮询 RX；内核 shell 行编辑；用户态 console stdin 同口（EAGAIN） |
| Shell | 提示符 `helix>`：`help` / `mem` / `page` / `int` / `uptime` / `halt` 等 |

### M6 dynamic path

```text
exec /bin/hello.dyn
  → elf_load_dynamic: PT_INTERP=/lib/ld-helix.so
  → map main PT_LOAD + interp PT_LOAD
  → user stack: argc/argv/env/aux (AT_ENTRY=main, AT_BASE=interp, …)
  → enter Ring3 at interp entry (0x50000000)
  → ld-helix jmp main → write("HelloDynOK\n"); exit

exec /bin/hello.musl
  → PT_INTERP=/lib/ld-musl-x86_64.so.1 + /lib/libc.so
  → PIE bias；HelloMuslDynOK
```

### M7–M8 networking

| 项 | 实现 |
|----|------|
| 驱动 | **e1000** 主路径；`virtio-net-pci` 后备 |
| L2/L3 | 以太网 + ARP + IPv4；静态 `10.0.2.15/24`，网关 `10.0.2.2` |
| ICMP | 内核 ping 网关 → **`HelixNetOK`** |
| UDP | `socket`/`bind`/`sendto`/`recvfrom`；本地回环 → **`user_udp_ok`** |
| hostfwd | MSYS2 QEMU 对已占 UDP 口无 SO_REUSEADDR；host ping 可超时（文档化） |

### M14 TCP

| 项 | 实现 |
|----|------|
| 状态机 | CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT(1/2) → TIME_WAIT → CLOSED；LISTEN/SYN_RECEIVED/CLOSE_WAIT/LAST_ACK |
| 数据结构 | `helix_tcp_sock`：seq/ack/window + RXQ(8) + TXQ(4)；`is_socket=2` |
| 收发 | `tcp_input`（IPv4 路径）→ demux by state；`sendto`/`recvfrom` 路由 by `is_socket` type（M15：TCP → `tcp_send_data`/`tcp_recv_data`） |
| 验收 | ICMP gate 通后 `tcp_init` → **`HelixTcpOK`** 内核自检；helixbox → **`HelixTcpUserOK`**（hostfwd TCP echo，M15） |

**约束**：协作调度下 TCP 阻塞操作用 EAGAIN + 用户态 yield 轮询。
QEMU user net hostfwd TCP：`hostfwd=tcp::8080-:8080`；host echo server 仅 smoke-net 启动。

### M9 graphics

| 项 | 实现 |
|----|------|
| 固件 | ExitBootServices 前 `LocateProtocol(GOP)`；选 ≥640 宽模式 |
| 驱动 | `fb_init` / `fb_cls` / `fb_pixel` / `fb_rect` / `fb_puts`；8×16 字体 |
| 验收 | `fb_smoke_done`；OVMF 4MB 常无 QemuVideoDxe → headless fallback OK |

### M13 signals

- **数据结构**：per-task `pending` 位图、`blocked` 位图、`sighand` 表（default/ignore/terminate）
- **syscall**：`kill`(62)、`rt_sigaction`(13)、`rt_sigprocmask`(14)
- **投递**：`signal_send` / `signal_deliver_current`（syscall 返回前检查）
- **SIGCHLD**：子 exit 时 `signal_on_exit` → 父 pending（default ignore）
- **SIGINT**：COM1 Ctrl+C → `signal_send` 前台 task（msh 兼容）
- **验收**：helixbox `HelixSigOK`（kill + SIGTERM）、msh 不崩溃

**协作调度下**：信号仅置位，不抢占；处理在 syscall 返回路径。SIGKILL 不可屏蔽，直接 terminate。

### M18 fb user-space interface + PS/2 keyboard

- **fb mmap**：`sys_mmap`(9) `fd==-4` 把 GOP 帧缓冲物理页 map 到 user VA（4 KiB 对齐页）；每页 remap 同一物理页
- **sys_fb_info**(546)：返回 `{width, height, pitch, bpp, size}` 到用户 struct（≥ 16 字节 buffer）
- **PS/2 键盘**：`kernel/drv/ps2.c` IRQ1 → scancode 队列（set-1 ASCII 翻译）+ `sys_readkey`(547) 非阻塞读
- **验收**：helixbox `HelixFBInfoOK` + `HelixFBMmapOK` + `HelixKbOK`（写入 fb 矩形 + 读键）

### M19 TUI shell (user-space)

- **二进制**：`bin/tui`（freestanding，无 libc，链接 `user/tui.ld` 加载至 0x40000000）
- **接口**：经 `sys_fb_info` → `sys_mmap(fd=-4)` → 直接写 BGRA 像素；`sys_readkey` 取 PS/2 键盘
- **渲染**：内嵌 8×16 VGA bitmap font（95 ASCII 字符，MSB-first）；文本网格（最大 80×30 @ 1024×768）；满行 `scroll_up()`
- **输入**：行编辑器支持 backspace / Ctrl+D (exit) / Ctrl+C (exit) / Enter
- **命令**：`help` / `clear` / `echo` / `ls` / `cat` / `ps` / `tcpstat` / `time` / `reboot` / `exit`（占位命令提示 "not implemented in M19"）
- **重启**：`xor %rax,%rax; mov %rax,%cr3` 触发三重故障
- **启动**：内核 shell `tui` 命令经 `task_exec_path("tui", "/bin/tui", av)`
- **验收**：内核 shell 启动 tui 后 `task_exec_path(tui) -> 0x...`；fb 缺失环境下 `[tui] no framebuffer` 优雅退出

### M23 PS/2 mouse

- **复用 PS/2 控制器第二通道**（`kernel/drv/ps2.c`）：同一 8042 芯片，port 0x64 命令 + 0x60 数据，命令字节 0xD4 前缀路由到 aux（鼠标）
- **aux 初始化序列**：`0xA8` (enable aux) → `0x60 0x47` (command byte enable IRQ1+IRQ12+sys) → `0xD4 0xF4` (data reporting, ACK 0xFA) → `0xD4 0xF3 0x64` (sample rate 100/sec)
- **3-byte packet 解析**：`[buttons|overflow, dx, dy]`；IRQ12 handler 累积到 `g_ms_packet[3]`，满 3 bytes 解析 → `{dx, dy, buttons}` → ring buffer (64 entries)
- **y 轴翻转**：PS/2 y+ = 屏上 → 用户态 dy>0 = 屏下（GUI 约定）；dx/dy signed 8-bit 扩展为 i16
- **overflow flag (bit 6/7)** 直接丢包，避免 wrap 误判；ring buffer 满时丢新事件（不阻塞 IRQ12）
- **sys_mouse_read**(548)：从 ring buffer 一次性 drain 到用户 `struct helix_mouse_event[]`；空时返回 `-EAGAIN` (-11)
- **验收**：串口含 `[ps2] mouse ready (IRQ12 unmasked)`；helixbox 探针走通 `HelixMouseOK`（有鼠标移动时追加 "(events)"）
- **QEMU headless**：无鼠标移动时 ring buffer 始终空，驱动 ready 视为通过；启用 `-display gtk/sdl` 后 IRQ12 触发

### M24 POSIX file ops 收尾

- **`struct vfs_ops` 扩展**：从 9 字段扩到 14 字段（加 `poll / unlink / rmdir / rename / fsync`），总 112 字节。所有 `.rdata` 里 ops 字面量必须 re-compile（fat/ramfs/pipe/vfs），否则 size mismatch 链接后 garbage data 跑偏
- **`sys_poll` / `sys_ppoll` (Linux NR 7/271)**：user `helix_pollfd[]` 数组；每个 fd 调 `vfs_poll_one`；fd<0 → revents=0，fd 不存在 → POLLNVAL；不阻塞（timeout 忽略）
- **`vfs_poll_one` default**：按 file 类型返回 mask (stdin → POLLIN, stdout → POLLOUT, dir → POLLIN, regular → POLLIN|POLLOUT，size=0 清 POLLIN)；`& events` 收窄
- **`sys_unlink / sys_rmdir / sys_rename` (87/84/82)**：`resolve_user_path` (cwd-aware) → `vfs_*` → FAT / ramfs ops 分发
- **FAT delete 实现**：`dir_unlink_at` mark `e[0]=0xE5` + `fat_free_chain(first_cluster)`；FAT16 root region 用 `g_fat.root_lba` + sec_i，FAT32 subdirs 走 cluster chain
- **FAT rename same-dir only**：`fat_rename_path` 检查 `old_parent == new_parent` 后 `memcpy(e, new_name83, 11)`；cross-dir 留 M25+
- **ramfs unlink/rmdir/rename**：`node_release(kfree data + memset slot)` + `node_child_count` (linear scan)；rename 同父 + 目标不存在约束
- **`sys_fsync / sys_fdatasync` (74/75)**：验证 fd；FAT `write_sector` 同步走 AHCI + ramfs in-memory → 实际 no-op；保留 op 字段为未来 buffered fs 留口
- **`O_TRUNC` 在 `fat_open`/`ramfs_open`**：原 M20 已实现（`node->size = 0` / FAT 现有 truncate 路径）
- **silent ENOSYS default**：dispatch default 分支去掉 `kprintf` 刷屏，统一返回 `ERR(ENOSYS)`；`grep "\[syscall\] ENOSYS" serial.log` 应为空
- **验收**：helixbox `HelixPollOK` (poll fd=0) + `HelixUnlinkOK` (create + unlink + rmdir + rename) + `HelixFsyncOK` (fsync + fdatasync + bad-fd EBADF + O_TRUNC 重置 size 0)

### M22 抢占式调度（preemptive）

- **协作之上叠加 IRQ0 tick 抢占点**：`g_preempt_pending` 每 tick 累加；syscall 返回路径检查
- **双 gate**：`task_count_alive() > 1`（单任务零开销）+ `timer_preempt_pending() >= PREEMPT_THRESHOLD(8)`（≈80ms 节流到 ~12 次/秒）
- **不调 `net_poll()`**：`sys_yield()` 内部已调；抢占路径只负责 `task_yield()` 让后台 READY task 推进
- **单任务场景**：`task_count_alive` short-circuit 后整个抢占逻辑零成本
- **验收**：helixbox `HelixPreemptOK`（fork 心跳子进程写 20 dots，主 task yield 30 次，期间 2~3 次抢占让 child 推进）

### 路线 D — 收尾修复补全

#### D1 smoke-net 闭环

- **Makefile harness**：启动 `tcp_echo_server.py` 后轮询 8080 端口直到 bind（替代 racey `sleep 8`）；
  server stderr → `tcp_echo_server.log` 方便排错
- **Greps 升 hard-fail**：`HelixTcpUserOK` / `HelixTcpPassiveOK` 从 soft-warn 升 `exit 1`，
  之前缺失仍返回 0 是 baseline 永远"假装通过"的根因
- **TCP max-retries log 节流**：kernel `static u64 s_last_retransmit_log + timer_ticks()`，
  每 30s 最多 log 一次；serial.log 不再被刷屏淹没让 grep 可信
- **验收**：`make smoke-net` EXIT=0 + 串口含所有 4 个 TCP/Net marker（真从 guest→host echo 收回复）

#### D2 mkdisk.py FAT32 nested dir

- **Bug**：`build_fat32_volume` root 路径对 `kind=="dir"` 分配 fresh `new_root` cluster 写
  dirent，但原始 `root_cl` 永远是全 0；OVMF 找不到 `EFI/BOOT/BOOTX64.EFI`
- **Fix**：`materialize_dir` 加 `is_root` flag（root 不写 `.`/`..`，FAT spec 禁止）；
  56 行 root children loop 替换为单行 `materialize_dir(tree, 0, root_cl, is_root=True)`
- **验证**：mtools `mdir -/ -i ... ::EFI/BOOT` 列出 `BOOTX64.EFI`；FAT16 smoke 不回归

#### D3 msh 行规程增强

- **ps2 0xE0 prefix**：之前 bit 7 set 被 `sc & 0x80` 滤掉，0xE0 直接被丢；现在接 0xE0 设
  `g_e0_pending` flag，等下一个 byte 翻译成 ESC [ A/B/C/D（xterm 箭头键标准序列）
- **msh_readline 重写**：cursor 模型 + 16 history ring + Ctrl+A/E/W/U/C；ESC 状态机
  解析箭头键；ANSI `\033[C` 推进光标（serial log 不支持但不影响 grep）
- **不引入新 syscall**：sys_read 路径不变（`g_cons_ops.read` 已 wired 到 `cons_read`）；
  PS/2 ring buffer 直接消费 ESC 序列

### M21 FAT32 完善

- **`fat_free_chain(c)`**：从 cluster `c` 沿 FAT chain 释放（entries → 0），EOF 停止
- **`root_unlink_and_free(name83)`**：在 root 中找 8.3 匹配项，mark 0xE5 + 调 `fat_free_chain`
  - FAT16：扫描固定 root region（`root_lba..root_lba+root_sectors`）
  - FAT32：遍历 root cluster chain（每 cluster `spc` 个 sector）
- **`fat_selftest_write`** stale-cleanup：改走 `root_unlink_and_free`（此前 FAT16-only，导致 FAT32 二次启动 stale dirent 残留）
- **`mkdisk.py`** 新增 `build_fat32_volume()`：reserved=32、4-byte FAT entries、BPB root_clus/fsinfo（sector 1）/bkbootsec（sector 6）/fatsz32、EOF mark `0x0FFFFFF8`、FSInfo 签名 `0x41615252` / `0x61417272`；ESP size ≥ 64 MiB 自动选 FAT32
- **验收**：`make smoke-fs`（FAT16 不回归）+ 64 MiB FAT32 大盘镜像两次连 boot → `selftest OK (FAT32): HelixFATWriteOK`，HELIXW.TXT cluster 两次都是 3（重新分配）
- **已知限制**：`build_fat32_volume` 嵌套子目录 materialization 有 bug。FAT32 验证路径用 mformat + mcopy 注入 flat root（避免嵌套递归）

### M20 — VFS ext + 用户态补全

- **FAT subdir 完整**：
  - `kernel/fs/fat.c` 加 `struct fat_dir_iter { u16 clus; u8 sec; u16 off; u16 eof; }`；`fs_priv` 存 dir state
  - `fat_resolve` 加 `u8 *out_attr` 参数；leaf 是 dir 时返回 0 + cluster + attr（之前返回 -1）
  - `fat_open` 检测 `attr & 0x10` 时创建 `vfs_file` with `is_dir=1` + `fs_priv=fat_dir_iter`，不走 `fat_file` 路径
  - `fat_getdents64` 重写：subdir 用 `it->clus` + `clus_to_lba` + `fat_get()` 沿 chain walk；
    EOF mark (`0x0FFFFFF8`/`0xFFF8`) 触发 break；FAT16 root 保留 fixed-region 路径
- **Permission syscall 显式 ENOSYS**：
  - `kernel/proc/syscall.c` 在 `case 59 execve` 后加 `case 21/90/91/92/93/94/132/133/280` → `ret = ERR(ENOSYS); break;`
  - 绕过 default 分支的 `kprintf` 刷屏；BusyBox 收到 ENOSYS 走标准错误路径
- **FAT 深嵌套子目录验证**：
  - `scripts/mkdisk_deep.sh` 构建 `out/helix-deep.raw.img`（raw FAT volume，mtools 可读）
  - 4 级目录 `a/b/c/d/file.txt` + 每级一个 levelN.txt
  - `mkdisk.py` 加 `--add-tree HOSTDIR::`（递归 walk）和 `--raw-fat`（输出无 GPT 包装的 FAT volume）
  - mtools `mdir -/ -i out/helix-deep.raw.img ::a/b/c/d` 列出 file.txt；`mtype` 输出 `HELIX_DEEP_OK`
- **/etc 资产 stage**：`esp_assets/passwd` (root entry) + `esp_assets/welcome.txt`
  (`HELIX_WELCOME_OK\n`) → `mkesp.sh` 写到 ESP `/etc/`
- **helixbox subdir probe**：`cmd_smoke` 加 `ls /etc` + `cat /etc/passwd` + `cat /etc/welcome.txt` + `ls /lib` 4 个探针，验证 kernel subdir getdents64 + open
- **BusyBox 多 applet 真实 smoke**：
  - `kernel/proc/exec.c` `linux_compat_run_busybox_applets` 用 `g_bb_applets[][16]`
    5-applet 表 + 模块级 `g_bb_idx`
  - chain: `echo HelixBusyBoxOK` → `cat /etc/welcome.txt` → `echo BB2_OK` → `true` → `echo HELIX_BB_DONE`
  - 每个 applet exit → `task_set_exit_all_hook(self)` 递归 → next applet
  - 末尾 → `msh_compat_run_smoke` → `linux_compat_run_helixbox`
  - **Heap bump**：`kernel/mm/heap.c` `HEAP_PAGES 1024 → 2048` (4→8 MiB) — 容纳 5 次 BusyBox ELF 重 load (each ≈ 1.1 MiB)
- **msh 增强 (6 builtin + `;` + alias expansion)**：
  - `user/msh.c` 加 `bi_alias` / `bi_unalias` / `bi_export` / `bi_unset` /
    `bi_test` (含 `[` 形式 + `-f/-e/-z/-n` unary + `=/!=` binary + 整数 `-eq/-ne/-lt/-gt`) / `bi_type`
  - 文件静态 `msh_aliases[16]` + `msh_envtab[32]` 表 (linear scan)
  - **`msh_exec_line` 重构**：拆 `msh_exec_line` (切 `;`) + `msh_exec_pipeline` (切 `|`) + `msh_strtok_r` (strtok_r-lite，skip empty tokens)
  - **alias expansion** in `msh_exec_pipeline`：lookup alias body → 拼接 `body + ' ' + tail_argv[1..]` → re-tokenize → 替换 argv0
  - **bi_test bug fix**：单参 `-f/-e/-z/-n` 操作符优先于 binary `=` 检查 (避免 `test -f FILE` 误报 "need binary expr")


## Subsystems (target shape)

| 子系统 | 起步里程碑 | 备注 |
|--------|------------|------|
| 串口日志 / kprintf | M0–M1 **done** | COM1；ConOut 仅 BS 前 |
| 物理内存 / 堆 | M1 **done** | bitmap PMM + **8MiB heap** (M20 bump for BusyBox chain) |
| 页表 / IDT | M1 **done** | identity 2MiB；异常→panic |
| 中断 / 时钟 | M2 **done** | 8259 + PIT；IRQ 可返回 |
| 内核 shell | M2 **done** | COM1；help/mem/page/int/… |
| 用户态 / syscall | M3 **done** | Ring3 + write/yield/exit + 协作 |
| VFS / FAT | M4 **done (RW 根)** | AHCI + GPT + FAT16 写；盘上 ELF |
| Linux 兼容子集 | M5 **done** | helixbox + 可选 BusyBox；uname=Helix |
| 动态链接 | M6 **done** | ld-helix + 真 musl |
| 网络 | M7–M8 **done（最小）** | e1000 + ICMP + UDP |
| 图形 | M9 **done** | GOP fb + headless fallback |
| 进程 | M10–M12 **done** | fork/exec/wait/pipe/cwd/msh |
| msh 增强 | **M20 done** | 6 builtin (alias/unalias/export/unset/test/type) + `;` 分隔符 + alias expansion |
| 信号 | **M13 done** | kill/sigaction/SIGCHLD/SIGINT |
| TCP | **M14–M17 done** | state machine + socket/connect/listen/accept；sendto/recvfrom 路由（M15）；sendmsg/recvmsg + passive hostfwd（M16）；TXQ retransmit（M17） |
| TUI | **M19 done** | 用户态 fb mmap + PS/2 键盘 → mini-terminal；helix shell `tui` 启动 |
| FAT32 | **M21 done** | FAT32 mount/写路径 + stale-cleanup helper；ESP ≥ 64 MiB 自动选 FAT32 |

## ABI policy

- **文件格式**：ELF（用户态）；引导产物为 PE/COFF EFI。
- **系统调用**：x86_64 Linux 号为主表；偏差记入 `SYSCALLS.md`。
- **`uname`**：默认 **Helix**（诚实）。
- **Windows / Win32**：非目标。

## Source layout

```text
boot/                 UEFI 入口与 handoff（efi_main，GOP）
kernel/
  ke/                 early main, shell
  mm/                 pmm, heap, vmm
  arch/x86_64/        paging, gdt, idt, isr, pic, pit, timer, irq, syscall_entry
  drv/                blk_ahci, e1000, virtio_net, fb
  net/                nic + eth/ARP/IPv4/ICMP + UDP
  fs/                 fat, vfs, ramfs, pipe
  proc/               elf, syscall, task, userland, exec, signal
user/                 init/task2/helixbox/ld-helix/hello.dyn/msh
libk/                 serial, kprintf, panic, string
include/
  efi/                最小 UEFI 类型
  helix/              公共头
  generated/          嵌入 user ELF 头（构建生成）
img/                  Logo PNG
third_party/          BusyBox / musl-dyn 等（fetch）
scripts/              mkdisk / run-qemu / elf_set_interp / check-deps
docs/                 ARCHITECTURE / BUILD / ROADMAP / SYSCALLS / GOAL_*
```

## Non-goals (reminder)

- 不基于 Linux 内核源码，不做发行版套壳  
- 不做 Win32/Wine  
- 不做自研 CPU  
- 早期不追求完整 glibc 桌面  
- 不把“能跑某个第三方工具”写成官方收录或世界排名  
