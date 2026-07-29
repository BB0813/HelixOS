# HelixOS

**本项目仅为整活 并不考虑引起任何言论冲突**
**“自研”内核** · **主兼容 Linux 用户态** · x86_64 · UEFI only

HelixOS 不是 Linux 发行版，也不基于 Linux 内核源码。内核（Helix）自研；用户态以 Linux ABI 兼容为第一公民，目标是逐步跑通 musl / BusyBox 级别的静态程序，再加深。

| 层 | 说明 |
|----|------|
| 自研 App / 调试工具 | 可选极小自研 API |
| Linux 用户态 | musl、BusyBox 等（第三方，原许可） |
| Linux syscall 兼容层 | 主路径，文档见 `docs/SYSCALLS.md` |
| **Helix 内核** | 调度 · 内存 · VFS · 驱动 · IPC |
| 硬件 | x86_64 + UEFI |

> 诚实边界：我们写的是自研内核 + Linux 兼容层；不是“又一个 Linux”。第三方用户态（BusyBox 等）单独标注许可与来源。

## 当前状态

**M5 — Linux 兼容子集**（已完成：helixbox 路径）

- M0–M4：启动、shell、Ring3、只读 FAT/VFS
- M5：Linux 号 syscall、`/bin/helixbox`、诚实 `uname=Helix`、**`/tmp` ramfs 可写 + mkdir**
- 验证：`make smoke-linux`（含 `tmp_write_ok`）

## 快速开始

### 依赖

| 工具 | 用途 | 推荐安装 |
|------|------|----------|
| Clang + LLD | 编译/链接 freestanding PE EFI | MSYS2: `mingw-w64-x86_64-clang` `mingw-w64-x86_64-lld` |
| Make | 构建 | MSYS2: `mingw-w64-x86_64-make` 或系统 make |
| QEMU | 仿真 | MSYS2: `mingw-w64-x86_64-qemu` 或官方 QEMU |
| OVMF | UEFI 固件 | 随 QEMU 包或单独下载（见 `docs/BUILD.md`） |
| NASM | 后期 asm（M0 可选） | `mingw-w64-x86_64-nasm` |

MSYS2 MinGW64 一键示例：

```bash
pacman -S --needed mingw-w64-x86_64-clang mingw-w64-x86_64-lld \
  mingw-w64-x86_64-make mingw-w64-x86_64-qemu mingw-w64-x86_64-nasm
```

Linux / WSL2：

```bash
# Debian/Ubuntu 示例
sudo apt install clang lld make qemu-system-x86 ovmf nasm
```

检查依赖：

```bash
make check-deps
# 或
./scripts/check-deps.sh
```

### 构建与运行

```bash
make && make smoke-linux   # M5 applets
make smoke-fs              # 盘 FS
make smoke-user            # Ring3 协作
```

串口可见：

```text
[user] HelixLinuxOK
[user] HelixFS OK
[user] Helix
[user] sh_ok
[user] helixbox_smoke_done
```

详细步骤与排错见 [`docs/BUILD.md`](docs/BUILD.md)。

## 仓库结构

```text
HelixOS/
├── boot/           # UEFI handoff
├── kernel/         # ke / mm / arch / drv(ahci) / fs(fat,vfs) / proc
├── libk/
├── user/           # freestanding init/task2
├── esp_assets/     # hello.txt 等写入 ESP 的测资
├── scripts/        # mkdisk / mkesp / run-qemu / smoke-*
├── docs/
└── Makefile
```

## 里程碑

见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。顺序：M0 → M1 → … → M5（Linux 兼容子集兑现）。

## 许可

- 自研代码：默认 **MIT**（见 `LICENSE`）
- 第三方：各自原许可；BusyBox 等 GPL 组件仅放 `third_party/`，并附合规说明

## 文档

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — 分层与原则
- [`docs/BUILD.md`](docs/BUILD.md) — 工具链与构建
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — 里程碑
- [`docs/SYSCALLS.md`](docs/SYSCALLS.md) — Linux 兼容 syscall 表（M5 起填充）
