# HelixOS

<p align="center">
  <img src="img/Helix无背景Logo.png" alt="HelixOS" width="160"/>
</p>

**本项目为整活向 玩具级自研OS 借鉴于XJ380OS**

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

**M0–M24 + 路线 A→B→C→D (D1–D7) 已完成**（2026-08-08）；剩余：D4.2 真实 munmap/mprotect（需 per-task PML4 + COW，M25+）

| 阶段 | 内容 | 关键标记 |
|------|------|----------|
| M0–M3 | UEFI、早期核、shell、Ring3 协作 | `M3 userland OK` |
| M4 | **FAT16/FAT32 可写**（AHCI 持久化）+ VFS + `/tmp` ramfs | `HelixFATWriteOK` |
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
| M18 | fb mmap user-space + PS/2 keyboard | `HelixFBInfoOK` / `HelixFBMmapOK` / `HelixKbOK` |
| M19 | **TUI shell**（fb + PS/2 键盘 → `bin/tui`） | `task_exec_path(tui) -> 0x...` |
| M21 | **FAT32 完善**（stale-cleanup helper + 64MiB FAT32 大盘验证） | `selftest OK (FAT32): HelixFATWriteOK` |
| M22 | **抢占式调度**（IRQ0 tick → syscall 返回路径 task_yield，阈值 8） | **`HelixPreemptOK`** |
| M23 | **PS/2 鼠标**（aux port + IRQ12 + 3-byte packet → sys_mouse_read(548)） | **`HelixMouseOK`** |
| M24 | **POSIX file ops 收尾**（poll/ppoll + unlink/rmdir/rename + fsync/fdatasync + silent ENOSYS；**M24.1 cross-dir rename**） | **`HelixPollOK` / `HelixUnlinkOK` / `HelixFsyncOK` / `HelixRenameOK`** |
| **D1** | **smoke-net 闭环**（端口等待 + HelixTcpUserOK/PassiveOK hard-fail + max-retries 节流） | **`HelixTcpUserOK` + `HelixTcpPassiveOK`** |
| **D2** | **FAT32 nested dir 修复**（`is_root` flag + root 走 `materialize_dir`） | EFI/BOOT/BOOTX64.EFI 可达（mtools mdir 验证） |
| **D3** | **msh 行规程增强**（ps2 0xE0 + ESC 箭头键 + cursor/history/Ctrl+A-E-W-U） | `make` EXIT=0；`smoke-linux` EXIT=0 |
| **D4** | **内存安全基础设施**（paging unmap/prot + no-op stubs + 共享 PML4 限制文档化） | **`HelixMunmapOK` / `HelixMprotectOK`** |
| **D5** | **熵源 + heap coalesce**（RDRAND/LFSR getrandom + kfree 双向 full coalesce + execve argv 无 leak 审查） | **`HelixGetrandomOK` / `HelixMallocOK`** |
| **D6** | **UI/UX 清理**（mm_layout.h 集中地址常量 + fd_init_task_stdio 入口集中） | `make` EXIT=0；smoke 全 pass |
| **D7.1** | **mkesp.sh 硬失败**（缺失 user ELF → exit 1 + 明确报错，替代静默跳过） | `rm build/user/*.elf && mkesp.sh` 非零退出 |
| **D7.2** | **FAT stat 真实化**（CMOS RTC + FAT dirent 时间戳 + st_ino/mtime/atime/ctime） | **`HelixStatOK`** |
| **M20** | **VFS ext + 用户态补全**（FAT subdir 走 cluster chain + chmod/chown/utimes 显式 ENOSYS + 4 级 subdir 验证 + /etc 资产 + **BusyBox 5-applet 真实 chain** + **msh 6 builtin** `alias`/`unalias`/`export`/`unset`/`test`/`type` + `;` 分隔符 + alias expansion + heap 4→8MiB） | `BusyBox chain done (5 applets)` + `HELIX_MSH_ALIAS_OK` + `HELIX_MSH_EXPORT_OK` + `HELIX_MSH_TEST_OK` + `HELIX_MSH_DONE` |

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
selftest OK (FAT32): HelixFATWriteOK
HelloMuslDynOK
HelixNetOK
user_udp_ok
ForkChildOK  ForkParentOK
PipeOK  WaitOK
HelixMshOK
HelixCwdOK
HelixSigOK
HelixPreemptOK
HelixTcpOK
HelixTcpUserOK
HelixTcpPassiveOK
helixbox_smoke_done
HelixFBInfoOK  HelixFBMmapOK  HelixKbOK
HelixMouseOK
HelixRenameOK
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
├── user/           # init, task2, helixbox, ld-helix, hello.dyn, msh, tui
├── img/            # Logo（深色 / 浅色 / 无背景）
├── esp_assets/     # 写入 ESP 的测资
├── scripts/        # mkdisk / mkesp / run-qemu / smoke-*
├── docs/           # ARCHITECTURE / BUILD / ROADMAP / SYSCALLS / GOAL_*
└── Makefile
```

## 里程碑

见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。顺序：M0 → … → **M24** + **路线 D**（D1–D7 已完成；剩余 D4.2 per-task PML4 — 修 issue [#1](https://github.com/BB0813/HelixOS/issues/1) Sakura 扫描的 4 个 CRITICAL）。

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
