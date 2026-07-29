# Goal: HelixOS M3 — 用户态与进程

> **状态：已完成（2026-07-28）** — GDT/Ring3、`syscall` write/yield/exit、嵌入 ELF、协作调度；  
> 验证：`make smoke-user`。本文件保留为历史 goal 提示词。
>
> 实现：`kernel/proc/{elf,syscall,task,userland}.c`、`kernel/arch/x86_64/{gdt,syscall_entry}*`、`user/`。

## 原目标摘要
Ring3 + write/exit/yield + 静态 ELF + 协作多任务 + init；`smoke-user` 与文档。

完整原始 goal 正文见会话历史；验收已满足。
