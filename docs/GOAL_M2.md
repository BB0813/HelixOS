# Goal: HelixOS M2 — 中断与内核壳

> **状态：已完成（2026-07-28）** — 实现见 `kernel/arch/x86_64/{pic,pit,timer,irq,idt}.c`、`kernel/ke/shell.c`；  
> 验证：`make smoke` / `make smoke-shell`。本文件保留为历史 goal 提示词。

## 项目
HelixOS：自研 x86_64 内核 Helix + Linux 用户态兼容（非 Linux 源码/发行版）。
UEFI only。M0/M1 已完成：`BOOTX64.EFI` 经 ExitBootServices 后有 identity 页表、IDT（异常→panic）、bitmap PMM、堆、`kprintf`；`make smoke` / `make smoke-panic` 通过。

## 本目标（M2 一次做完，可构建可验证）
在 M1 早期核之上实现可交互的内核态环境，QEMU+OVMF 可验证：

1. 时钟中断心跳（PIT 或 lapic timer 二选一，优先 QEMU 简单路径；固定频率即可）
2. 串口输入 **或** 键盘输入（至少一路；推荐 COM1 输入，与现有日志同口，便于无头 smoke）
3. 内核态简单命令行：至少 `help` / `mem` / `page` / `int`（可再加 `uptime`/`halt` 等，保持最小）
4. 基本中断统计（时钟 tick 计数；可选其它 IRQ 计数），`int` 命令可打印
5. IDT 从“仅异常 panic”扩展到可处理选定 IRQ（EOI 正确）；未处理 IRQ 不静默丢也不胡乱 panic
6. 保持 M1 验收不回归：`make smoke` 仍须看到 `Helix boot` + `ExitBootServices OK` + `M1 early kernel OK`
7. 文档同步：`docs/ROADMAP.md` 勾选 M2；`ARCHITECTURE.md` / `BUILD.md` / `README.md` 反映输入路径与命令表

## 约束
- 语言：C + 极少量 asm；freestanding；无 libc
- 构建：延续现有 Makefile / clang / lld-link；Windows/MSYS2 路径坑沿用 scripts（GPT 镜像，勿用 `fat:rw:dir`）
- 调试：QEMU + OVMF；**串口是唯一可靠日志与（若选串口输入）控制台**
- 不引入 Linux 内核源码；不做用户态 / syscall / VFS（那是 M3+）
- 少空谈，优先可合并增量；回复简体中文，标识符英文
- 中断上下文禁止重入复杂堆操作；kprintf 若在 IRQ 中使用须可重入或仅 tick 标志位

## 验收
- `make check-deps && make && make smoke` 通过（M0+M1 标记仍在）
- 交互或脚本化输入下：`help` 有命令列表；`mem` 显示 PMM/堆概要；`page` 显示页表/ceiling 类信息；`int` 显示 tick（及已实现的 IRQ 统计）
- 串口可见周期性心跳（例如每 N tick 一行，或 `uptime` 反映 tick），证明时钟 IRQ 在跑
- 无头验证策略写进 `docs/BUILD.md`（例如 `make smoke-shell`：通过 QEMU/chardev 喂入命令序列并 grep 输出）
- `docs/ROADMAP.md` M2 勾选完成

## 非目标（本轮不做）
- Ring3 / ELF / 多任务 / syscall
- VFS、磁盘读写、BusyBox
- 完整 APIC 拓扑、多核、校准高精度计时
- 图形 / 网络
- 抢占式调度（可只做 tick 计数；调度留 M3）

## 建议实现顺序
1. 读 `boot/`、`kernel/`、`libk/`、`Makefile`、`docs/*` 与 M1 handoff
2. 8259 或 IOAPIC 最小初始化 + 时钟源 + IRQ stub + EOI + tick
3. 串口 RX（或键盘）+ 行编辑（最小：收一行、回显）
4. shell 分发 `help/mem/page/int`
5. smoke / 文档 / 不回归 M1

## 开工
先读现有代码再改；保持单一 `BOOTX64.EFI` 链路除非有强理由拆分。阶段末给出验证命令与 `serial.log` 摘录。
