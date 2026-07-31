# Goal: HelixOS M0 — 仓库与工具链（历史）

> **状态：已完成** — 最小 freestanding UEFI 应用；`make smoke` 串口含 `Helix boot`。

## 范围

1. 项目命名：HelixOS / 内核 Helix  
2. 目录结构、README、LICENSE（MIT）、`.gitignore`  
3. `docs/ARCHITECTURE.md` `BUILD.md` `ROADMAP.md` `SYSCALLS.md` 初稿  
4. 最小 freestanding UEFI 应用（打印 Helix boot）  
5. Makefile + `scripts/`（deps / esp / qemu / mkdisk）  
6. `make` → `out/BOOTX64.EFI`（MSYS2 clang+lld）  
7. `make smoke` / QEMU+OVMF 串口横幅  

## 约束 / 非目标

- C + 极少 asm；freestanding；不依赖 gnu-efi / 完整 EDK2  
- 单一 PE EFI 映像  
- 不做页表 / 中断 / 用户态（M1+）  

## 验收

```bash
make check-deps && make && make smoke
# serial.log: Helix boot
```

## 实现要点

- `boot/efi_main.c`、`include/efi/`、顶层 `Makefile`、`scripts/`  
