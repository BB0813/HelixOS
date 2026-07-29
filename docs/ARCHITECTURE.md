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
Linux 用户态（musl, BusyBox, …）
        ↓ Linux syscall 兼容层（主路径）
Helix 内核（调度 · 内存 · VFS · 驱动框架 · IPC）
        ↓
x86_64 + UEFI + 硬件
```

### 原则

1. **一套内核语义**，尽量 POSIX 味，方便映射 Linux ABI。
2. Linux 兼容是 **syscall 号 + 必要的 `/proc` 假节点 + loader 行为**，不是重新实现 Linux。
3. 自研 API 保持极小，默认不与 Linux 抢第一公民地位。
4. 驱动优先 QEMU 友好：串口、GOP/framebuffer、键盘、virtio-blk/net。

## Boot path (M0 → M4, current)

```text
UEFI firmware (OVMF)
    → GPT disk / ESP FAT / BOOTX64.EFI
         boot → ExitBootServices → kernel_early_main
         M1: pmm / identity / gdt / idt / heap
         M2: PIC+PIT / shell
         M4: AHCI blk_read → GPT → FAT mount → VFS /
         M3/M4 userland:
           · read /hello.txt (smoke)
           · load /bin/init.elf + /bin/task2.elf (disk; embed fallback)
           · Ring3 cooperative tasks → idle shell
```

**内核仍是单一 EFI 映像**；测试 ELF 同时 **嵌入**（fallback）与 **写入 ESP**（主路径）。

### M4 storage

| 项 | 选择 |
|----|------|
| 块设备 | PCI class 01:06 AHCI，port0，同步 READ DMA EXT |
| MMIO | `paging_map_mmio`（ABAR 常在 identity 外） |
| 分区 | GPT，ESP type GUID |
| FS | `/` → FAT16 **只读**；`/tmp` → **ramfs 可写** |
| VFS | 路径分流：`/tmp…`→ramfs，其余→FAT |
| FD | 每任务 16 槽；0/1/2 = 串口 console |
| 写 | ramfs 上 mkdir/create/write；ESP 仍只读 |

### M3 user model

| 项 | 选择 |
|----|------|
| 地址空间 | 共享 CR3：identity + user 4K（U=1）+ MMIO |
| 进入用户 | `iretq`；syscall/`sysretq` |
| 调度 | 协作 yield/exit |
| 输出 | `write` → console → `[user] ` + 串口 |

### M1 memory notes

- `phys_ceiling` 只统计 RAM-like 类型，**不含** MMIO/Reserved（避免 QEMU 把 ceiling 拉到 TiB 级）。
- PMM 只把 `EfiConventionalMemory` 标为空闲；Loader/BootServices 区域暂不回收（映像、boot_info、旧栈仍可能在其中）。
- Identity map：`[0, phys_ceiling)` 2MiB pages；镜像与栈均在该范围内。

### M2 interrupts & shell

| 项 | 实现 |
|----|------|
| PIC | 8259 重映射 master=`0x20` slave=`0x28`；默认全屏蔽，按需 unmask |
| 时钟 | PIT ch0 mode3 ≈100Hz → IRQ0；`timer_on_irq` 只加计数/置位 |
| 心跳 | 主循环 `timer_poll_heartbeat` 打印 `[tick] N`（**不在 IRQ 里 kprintf**） |
| IDT | gates 0..47；0..31 异常 → panic；32..47 → `irq_handle` + EOI + `iretq` |
| 输入 | COM1 轮询 RX（非 IRQ）；行缓冲 + 回显 + 退格 |
| Shell | 提示符 `helix>`；命令见下表 |

**Shell 命令**

| 命令 | 输出 |
|------|------|
| `help` / `?` | 命令列表 |
| `mem` | PMM total/free/ceiling、boot conventional 概要 |
| `page` | identity map 上界、PMM/boot ceiling |
| `int` | timer ticks/hz + 非零 IRQ 计数 |
| `uptime` | ticks 与约略秒数 |
| `halt` | `cli; hlt` 死循环 |

## Subsystems (target shape)

| 子系统 | 起步里程碑 | 备注 |
|--------|------------|------|
| 串口日志 / kprintf | M0–M1 **done** | COM1；ConOut 仅 BS 前 |
| 物理内存 / 堆 | M1 **done** | bitmap PMM + 256KiB heap |
| 页表 / IDT | M1 **done** | identity 2MiB；异常→panic |
| 中断 / 时钟 | M2 **done** | 8259 + PIT；IRQ 可返回 |
| 内核 shell | M2 **done** | COM1；help/mem/page/int/… |
| 用户态 / syscall | M3 **done** | Ring3 + write/yield/exit + 协作 |
| VFS / FAT | M4 **done (RO)** | AHCI + GPT + FAT16；盘上 ELF |
| Linux 兼容子集 | M5 **done (helixbox)** | Linux syscall 号 + multi-call；uname=Helix |
| Linux syscall 表 | M5 | 见 `SYSCALLS.md` |
| 动态链接 / musl | M6 | mmap/brk/arch_prctl 等 |
| 网络 / 图形 | M7 | 可后置 |

## ABI policy

- **文件格式**：ELF（用户态）；引导阶段产物为 PE/COFF EFI。
- **系统调用**：以 x86_64 Linux syscall 编号为主表；偏差必须记入 `SYSCALLS.md`。
- **`uname` / os-release**：默认报告 Helix；若为兼容需要伪装 Linux，必须在文档标明“兼容用途”。
- **Windows / Win32**：非目标。

## Source layout

```text
boot/                 UEFI 入口与 handoff（efi_main）
kernel/
  ke/                 early main, shell
  mm/                 pmm, heap, vmm
  arch/x86_64/        paging, gdt, idt, isr, pic, pit, timer, irq, syscall_entry
  proc/               elf, syscall, task, userland
user/                 freestanding init/task2 + link scripts
libk/                 serial, kprintf, panic, string
include/
  efi/                最小 UEFI 类型
  helix/              公共头
  generated/          嵌入的 user ELF 头（构建生成）
 
user/                 自研测试/工具用户态（M3+）
third_party/          第三方源码与合规说明
scripts/              mkdisk / run-qemu / check-deps
docs/
```

## Non-goals (reminder)

- 不基于 Linux 内核源码，不做发行版套壳  
- 不做 Win32/Wine  
- 不做自研 CPU  
- 早期不追求完整 glibc 桌面  
- 不把“能跑某个第三方工具”写成官方收录或世界排名  
