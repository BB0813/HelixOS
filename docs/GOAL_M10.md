# Goal: HelixOS M10 — fork / execve（历史）

> **状态：已完成（2026-08-01）** — `ForkChildOK` + `ForkParentOK`（`make smoke-linux`）。

## 范围

1. `fork`(57)：复制 task + 内核栈 + **独立 PML4**；用户页物理复制；FD `fd_hold`。
2. `execve`(59)：VFS 读 ELF，替换用户空间 entry/stack/brk；argv 拷贝 + `setup_user_stack`。
3. `vmm_copy_user_page_tables`：4 级递归；2MiB 大页拆 4K 后逐页复制。
4. `user_pages[]` 跟踪（每任务最多 2048 页 / 8 MiB）。
5. helixbox smoke 自检 fork。

## 约束 / 非目标

- 仍协作调度；无 COW；无信号。
- 不实现完整 POSIX 进程组 / sessions。

## 验收

`make smoke-linux` 串口含 `ForkChildOK` + `ForkParentOK` + `helixbox_smoke_done`。
