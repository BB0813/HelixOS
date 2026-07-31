# Goal: HelixOS M1 — 启动与早期核（历史）

> **状态：已完成** — `ExitBootServices OK` + `M1 early kernel OK`；`make smoke-panic` → `HELIX PANIC` / `#PF`。

## 范围

1. UEFI MemoryMap；`ExitBootServices`（失败重试；切自有栈）  
2. Identity-map 页表（2MiB large pages，覆盖 RAM ceiling）  
3. IDT + 异常 0–31 → `panic`（`#PF` 打印 CR2）  
4. 物理页分配器（bitmap，仅 ConventionalMemory 空闲）  
5. 内核堆 `kmalloc`/`kfree`（早期 first-fit）  
6. COM1 `kprintf` / `panic` / `assert`（BS 前后均可用）  
7. 边界：`boot/` 仅 UEFI handoff；`kernel/` + `libk/` 为早期核  

## 约束 / 非目标

- 仍单一 EFI 映像  
- GOP/图形后置（M9）  
- 不做 IRQ 可返回 / shell（M2）  
- 不做用户态（M3）  

## 验收

```bash
make smoke          # ExitBootServices OK · M1 early kernel OK
make smoke-panic    # HELIX PANIC · #PF
```

## 实现要点

- `boot/efi_main.c` handoff · `kernel/mm/{pmm,heap}.c` · `kernel/arch/x86_64/{paging,gdt,idt}.c` · `libk/*` · `kernel/ke/main.c`  
