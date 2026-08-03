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
| FS | `/` → **FAT16 可写**（根目录 create/write/mkdir；写回 ESP 镜像）；`/tmp` → **ramfs 可写** |
| VFS | 路径分流：`/tmp…`→ramfs，其余→FAT；相对路径经 per-task cwd 解析（M12） |
| FD | 每任务 16 槽；0/1/2 = 串口 console（静态，不 free）；`refcount` + `fd_hold`（M11） |
| 限制 | FAT32 写路径未做；FAT 写主要覆盖根 8.3 |

### M3 / M10–M12 process model

| 项 | 选择 |
|----|------|
| 地址空间 | **每任务独立 CR3/PML4**（M10）：kernel 映射共享，用户页独立物理复制 |
| 进入用户 | `iretq`；`syscall` / `sysretq` |
| 调度 | **协作** yield/exit；syscall 内 `task_yield` **不会**真正切换（仅在 syscall 返回路径切换） |
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


## Subsystems (target shape)

| 子系统 | 起步里程碑 | 备注 |
|--------|------------|------|
| 串口日志 / kprintf | M0–M1 **done** | COM1；ConOut 仅 BS 前 |
| 物理内存 / 堆 | M1 **done** | bitmap PMM + 256KiB heap |
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
| 信号 | **M13 done** | kill/sigaction/SIGCHLD/SIGINT |
| TCP | **M14–M16 done** | state machine + socket/connect/listen/accept；sendto/recvfrom 路由（M15）；sendmsg/recvmsg + passive hostfwd（M16） |

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
