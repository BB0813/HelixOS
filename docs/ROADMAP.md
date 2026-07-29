# Roadmap

按顺序推进；**完成一个里程碑的可构建/可验证增量再开下一个**。

状态：`[ ]` 未做 · `[~]` 进行中 · `[x]` 完成

---

## M0 — 仓库与工具链 `[x]`

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

## M6 — 动态链接与 musl `[x]`（最小，非完整 musl ld.so）

Goal（历史）：[`docs/GOAL_M6.md`](GOAL_M6.md)

- [x] 动态 ELF：`PT_INTERP` + 自研 **`ld-helix`** + **`hello.dyn`** + auxv（`AT_BASE`/`AT_ENTRY`/…）
- [x] `mmap`（匿名 + FIXED/hint）/`munmap`/`mprotect`/`arch_prctl` 等必要项
- [x] `make smoke-dyn` → **`HelloDynOK`**
- [x] `SYSCALLS.md` / `ARCHITECTURE` / `BUILD` 同步

**验证**：`make smoke-dyn`。完整 musl 动态程序：工具链具备后可替换 ESP 上的 interp。

## M7 — 网络与图形（可后置） `[ ]`

- [ ] virtio-net 或等价
- [ ] 最小 TCP/UDP 或 tap 用户态
- [ ] framebuffer GUI 可选（不挡 CLI 生态）

---

## 文档债务（随里程碑）

- 每完成一阶段：更新本文件状态勾选与 `ARCHITECTURE.md` 中对应子系统
- M5 起 `SYSCALLS.md` 必须与代码同步
