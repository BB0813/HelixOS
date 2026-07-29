# Build & Run

## Targets

| 命令 | 作用 |
|------|------|
| `make` / `make all` | 编译链接 → `out/BOOTX64.EFI` |
| `make esp` | 组装最小 ESP 目录树到 `esp/` |
| `make run` | QEMU + OVMF 启动，串口接到终端并写 `serial.log`（可交互 shell） |
| `make smoke` | 无头启动；要求 M0+M1+M2 标记（含 `M2 shell ready` 与 `[tick]`） |
| `make smoke-shell` | TCP 串口喂 `help/mem/page/int/uptime`，grep 命令输出 |
| `make smoke-user` | 无头启动；要求 Ring3 `write` / `yield` / `exit` / `M3 userland OK` |
| `make smoke-fs` | AHCI+FAT 挂载、`HelixFS OK`、从盘加载 init/task2 |
| `make smoke-linux` | helixbox：echo/cat/ls/uname/sh 冒烟 |
| `make smoke-panic` | 带 `-DHELIX_M1_TEST_PF` 重建，断言 `#PF` panic，再恢复正常构建 |
| `make user` | 仅构建 `user/*.elf` 与 `include/generated/*_elf.h` |

## Toolchain

 freestanding **x86_64 UEFI** 应用：

- 编译：`clang -target x86_64-unknown-windows`（或等价）+ `-ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone`
- 链接：`lld-link` 或 `clang -fuse-ld=lld-link`，子系统 `EFI_APPLICATION`，入口 `efi_main`
- **不**依赖 gnu-efi / EDK2 完整 SDK（M0 自带最小 EFI 头）

### Windows（MSYS2 MinGW64）— 本仓库主验证路径

```bash
# 在 MSYS2 MinGW64 shell，或 Git Bash 且 PATH 含 /c/msys64/mingw64/bin
pacman -S --needed \
  mingw-w64-x86_64-clang \
  mingw-w64-x86_64-lld \
  mingw-w64-x86_64-make \
  mingw-w64-x86_64-qemu \
  mingw-w64-x86_64-nasm

export PATH="/c/msys64/mingw64/bin:$PATH"
cd /z/HelixOS   # 按你的盘符调整
make check-deps
make
make run
```

OVMF：MSYS2 的 QEMU 包通常把固件放在  
`/mingw64/share/qemu/` 或 `/mingw64/share/edk2-ovmf/`。  
`scripts/run-qemu.sh` 会按常见路径自动搜索；也可：

```bash
export OVMF_CODE=/path/to/OVMF_CODE.fd
export OVMF_VARS=/path/to/OVMF_VARS.fd   # 可选；没有则只用 CODE
make run
```

### Linux / WSL2

```bash
# Debian/Ubuntu
sudo apt update
sudo apt install clang lld make qemu-system-x86 ovmf nasm

# Fedora
sudo dnf install clang lld make qemu-system-x86 edk2-ovmf nasm

make check-deps
make
make run
```

常见 OVMF 路径：

- `/usr/share/OVMF/OVMF_CODE.fd` + `OVMF_VARS.fd`
- `/usr/share/edk2/ovmf/OVMF_CODE.fd`
- `/usr/share/qemu/OVMF.fd`（部分发行版合并镜像）

## Expected serial output (M2)

```text
[Helix] boot stub starting
HelixOS M1 — UEFI boot
Helix boot
…
[Helix] ExitBootServices OK
[Helix] === early kernel (post ExitBootServices) ===
[pmm] …
[paging] identity 0..0x… via 2MiB pages, CR3=0x…
[idt] loaded gates 0..47, CS=0x…
[heap] …
[Helix] M1 early kernel OK
[pic] remapped IRQs to 0x20..0x2f, all masked
[pit] ch0 mode3 ~100 Hz …
[timer] IRQ0 unmasked, hz=100
[irq] PIC+PIT ready (IRQ0 timer)
[shell] COM1 line editor ready
helix> …
[Helix] M2 shell ready (type help)
[tick] 100
[tick] 200
…
```

交互（`make run`）：在串口终端输入 `help` / `mem` / `page` / `int` / `uptime` / `halt`。

### `make smoke` / `make smoke-shell`

| 目标 | 做法 | 断言 |
|------|------|------|
| `smoke` | `HEADLESS=1` + 超时跑 QEMU，stdio 串口写 `serial.log` | `Helix boot`、`ExitBootServices OK`、`M1 early kernel OK`、`M2 shell ready`、`[tick]` |
| `smoke-shell` | `scripts/smoke-shell.sh`：QEMU 串口挂 **TCP** `127.0.0.1:4659`（`server=on,wait=off`），等 shell ready 后用 Python socket 发送 `help\r mem\r …` | 输出含 `Helix kernel shell`、`PMM: total_pages`、`Paging: identity map`、`timer: ticks=`、`uptime:` |
| `smoke-user` | 同 smoke，超时略长 | 另需 `Hello from Ring3`、`init: online`、`task2: hi`、`yield 1 -> 2`、`M3 userland OK` |
| `smoke-fs` | 同左 | `M4 fs ready`、`HelixFS OK`、`loaded init+task2 from disk`、用户输出 |
| `smoke-linux` | 同左，超时更长 | `HelixLinuxOK`、`HelixFS OK`、uname 行 `Helix`、`sh_ok`、`helixbox_smoke_done` |
| `smoke-panic` | `-DHELIX_M1_TEST_PF` 重建 | `HELIX PANIC` + `#PF` |

### ESP 测资（M4）

`make esp` → `scripts/mkesp.sh` 调用 `mkdisk.py`，除 `BOOTX64.EFI` 外可选加入：

| 主机路径 | ESP 路径 |
|----------|----------|
| `esp_assets/hello.txt` | `/hello.txt` |
| `build/user/init.elf` | `/bin/init.elf` |
| `build/user/task2.elf` | `/bin/task2.elf` |
| `build/user/helixbox.elf` | `/bin/helixbox` |

**M4 文件系统只读**（无写回 FAT）。**M5** 用户程序为自研 **helixbox**（MIT multi-call，Linux syscall ABI）。真实 BusyBox 静态构建见 `third_party/README.md`。

### 用户程序构建

```bash
make user
# → build/user/init.elf, task2.elf, helixbox.elf
# → include/generated/user_*_elf.h
```

工具：`clang --target=x86_64-unknown-none-elf` + `ld.lld` + `scripts/bin2hdr.py`。

> Windows/MSYS：无头 shell 用 `make smoke-shell`（TCP）；`mkdisk --add` 使用相对路径以便 native python 打开文件。

`make smoke-panic` 额外出现：

```text
[Helix] deliberate page-fault test
!!!! HELIX PANIC !!!!
exception #PF (14) err=0x2 rip=0x... cr2=0x...
```

**以串口为准**。退出 QEMU：关窗口，或 `Ctrl-A X`（nographic）。

## Layout of artifacts

```text
build/                 中间 .o
out/BOOTX64.EFI        最终 EFI 应用
out/esp.img            GPT 磁盘（内含 FAT ESP + BOOTX64.EFI）
esp/EFI/BOOT/…          staged 目录（供打包）
serial.log             最近一次 run/smoke 的串口记录
ovmf_vars.fd           OVMF 可变 NVRAM 工作副本
```

`scripts/mkdisk.py` 生成 **GPT + ESP 分区 + FAT16**；不要依赖 QEMU `fat:rw:dir`（在部分 Windows/QEMU 上会因临时文件路径失败）。

## Troubleshooting

| 现象 | 排查 |
|------|------|
| `clang not found` | PATH 是否含 MinGW64 bin；或 `make check-deps` |
| `lld-link not found` | 安装 `mingw-w64-x86_64-lld`；Linux 上可能是 `lld` 包提供 |
| `OVMF not found` | 设 `OVMF_CODE`；或安装 `ovmf` / `edk2-ovmf` |
| QEMU 黑屏无输出 | 看 `serial.log`；确认用了 `make run` 的串口参数 |
| 链接报 subsystem / entry | 确认用 `scripts` 里的链接标志，入口符号为 `efi_main` |
| 中文路径/空格问题 | 尽量在无空格路径构建 |
| `smoke-shell` 连不上 TCP | 端口被占可设 `HELIX_SERIAL_PORT=`；看 QEMU 是否仍在跑 |
| 管道喂命令无回显 | 用 `make smoke-shell`，勿依赖 stdio 管道 |

## Cross notes

- 开发机可以是 Windows + MSYS2（当前默认验证）、原生 Linux 或 WSL2。
- 不要求本机安装 EDK2 完整树。
- M1 之后会引入 `x86_64-elf` 风格内核 ELF；当前 M0–M2 仍是单一 PE EFI。
- M2 控制台 = COM1（日志与输入同口）。
