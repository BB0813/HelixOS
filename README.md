# HelixOS

<p align="center">
  <img src="img/Helix无背景Logo.png" alt="HelixOS" width="160"/>
</p>

**本项目为“中国第六 世界第七”自研OS 借鉴于人朝的小郭同学（XJ380OS）**

**该技术路线与XJ380OS的技术路线有高相似度 变相证明XJ380OS的路线没问题 但他们确实在宣发方面出现了严重失误 以导致出现了这种巨大的公关问题**

**本项目仅为整活 并不考虑引起任何言论冲突**  
**“自研”内核** · **主兼容 Linux 用户态** · x86_64 · UEFI only

HelixOS 不是 Linux 发行版，也不基于 Linux 内核源码。内核（Helix）自研；用户态以 Linux ABI 兼容为第一公民，目标是逐步跑通 musl / BusyBox 级别的静态程序，再加深。

| 层 | 说明 |
|----|------|
| 自研 App / 调试工具 | 可选极小自研 API |
| Linux 用户态 | musl、BusyBox、msh 等（第三方原许可 / 自研 MIT） |
| Linux syscall 兼容层 | 主路径，文档见 `docs/SYSCALLS.md` |
| **Helix 内核** | 调度 · 内存 · VFS · 驱动 · 网络 · 进程 |
| 硬件 | x86_64 + UEFI |

> 诚实边界：我们写的是自研内核 + Linux 兼容层；不是“又一个 Linux”。第三方用户态（BusyBox 等）单独标注许可与来源。

## 当前状态

**M0–M17 已完成**（2026-08-03）

| 阶段 | 内容 | 关键标记 |
|------|------|----------|
| M0–M3 | UEFI、早期核、shell、Ring3 协作 | `M3 userland OK` |
| M4 | **FAT16 可写**（AHCI 持久化）+ VFS + `/tmp` ramfs | `HelixFATWriteOK` |
| M5 | Linux 号 syscall、`helixbox`、可选 BusyBox | `HelixBusyBoxOK` / `helixbox_smoke_done` |
| M6 | `ld-helix` + **真 musl** | `HelloDynOK` / `HelloMuslDynOK` |
| M7 | e1000 + eth/ARP/IPv4/**ICMP** | **`HelixNetOK`** |
| M8 | **UDP socket** + TCP ENOSYS stubs | **`user_udp_ok`** |
| M9 | GOP 帧缓冲（OVMF 无驱动则 headless） | `fb_smoke_done` |
| M10 | `fork` / `execve`（独立 PML4） | `ForkChildOK` |
| M11 | `wait4` / `pipe` / FD refcount / **msh** | `PipeOK` · `HelixMshOK` |
| M12 | per-task **cwd** / `chdir` / 路径解析 / console stdin | **`HelixCwdOK`** |
| M13 | 信号最小集（SIGCHLD/SIGINT/kill + HelixSigOK） | **`HelixSigOK`** |
| M14 | **TCP 全栈**（state machine + connect/accept/listen） | **`HelixTcpOK`** |
| M15 | TCP 用户态 echo（sendto/recvfrom TCP 路由） | **`HelixTcpUserOK`** |
| M16 | sendmsg/recvmsg + TCP passive hostfwd echo | **`HelixTcpPassiveOK`** |
| M17 | TCP retransmission（TXQ 入队 + ACK 清除 + 定时器） | **`HelixTcpOK`** |

## 快速开始

### 依赖

| 工具 | 用途 | 推荐安装 |
|------|------|----------|
| Clang + LLD | 编译/链接 freestanding PE EFI | MSYS2: `mingw-w64-x86_64-clang` `mingw-w64-x86_64-lld` |
| Make | 构建 | MSYS2 make 或系统 make |
| QEMU | 仿真 | MSYS2 qemu 或官方 QEMU |
| OVMF | UEFI 固件 | 随 QEMU 包或单独下载（见 `docs/BUILD.md`） |

MSYS2 MinGW64：

```bash
pacman -S --needed mingw-w64-x86_64-clang mingw-w64-x86_64-lld \
  mingw-w64-x86_64-make mingw-w64-x86_64-qemu mingw-w64-x86_64-nasm
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
```

```bash
make check-deps
make && make smoke-linux   # fork/pipe/msh/cwd
make smoke-fs              # FAT 写回
make smoke-net             # ICMP + UDP
make smoke-musl            # 真 musl 动态
make smoke-fb              # 帧缓冲（无 GOP 则 headless OK）
```

串口关键标记示例：

```text
HelixFATWriteOK
HelloMuslDynOK
HelixNetOK
user_udp_ok
ForkChildOK  ForkParentOK
PipeOK  WaitOK
HelixMshOK
HelixCwdOK
HelixSigOK
HelixTcpOK
HelixTcpUserOK
HelixTcpPassiveOK
helixbox_smoke_done
```

详细步骤与排错见 [`docs/BUILD.md`](docs/BUILD.md)。

## 仓库结构

```text
HelixOS/
├── boot/           # UEFI handoff（GOP → boot_info）
├── kernel/
│   ├── ke/         # early main, shell
│   ├── mm/         # pmm, heap, vmm
│   ├── arch/x86_64/# paging, gdt, idt, pic, pit, syscall_entry
│   ├── drv/        # ahci, e1000, virtio_net, fb
│   ├── net/        # eth/ARP/IPv4/ICMP/UDP
│   ├── fs/         # fat, vfs, ramfs, pipe
│   └── proc/       # elf, syscall, task, exec, userland
├── libk/           # serial, kprintf, panic, string
├── user/           # init, task2, helixbox, ld-helix, hello.dyn, msh
├── img/            # Logo（深色 / 浅色 / 无背景）
├── esp_assets/     # 写入 ESP 的测资
├── scripts/        # mkdisk / mkesp / run-qemu / smoke-*
├── docs/           # ARCHITECTURE / BUILD / ROADMAP / SYSCALLS / GOAL_*
└── Makefile
```

## 里程碑

见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。顺序：M0 → … → **M16**（已完成）；下一候选 **M17 TCP 重传 / fb 用户态接口**。

## 许可

- 自研代码：默认 **MIT**（见 `LICENSE`）
- 第三方：各自原许可；BusyBox 等 GPL 组件仅放 `third_party/`，并附合规说明

## 文档

| 文档 | 内容 |
|------|------|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 分层、启动路径、子系统 |
| [`docs/BUILD.md`](docs/BUILD.md) | 工具链、QEMU、smoke 目标 |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | 里程碑勾选与验收 |
| [`docs/SYSCALLS.md`](docs/SYSCALLS.md) | Linux 兼容 syscall 表 |
| `docs/GOAL_M*.md` | 各阶段历史 goal 提示词 |

## Logo

| 文件 | 用途 |
|------|------|
| [`img/Helix无背景Logo.png`](img/Helix无背景Logo.png) | README / 透明底 |
| [`img/Helix深色Logo.png`](img/Helix深色Logo.png) | 浅色背景 |
| [`img/Helix浅色Logo.png`](img/Helix浅色Logo.png) | 深色背景 |
