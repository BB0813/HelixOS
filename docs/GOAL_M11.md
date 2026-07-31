# Goal: HelixOS M11 — waitpid / pipe / msh（历史）

> **状态：已完成（2026-08-01）** — `PipeOK` · `WaitOK` · `HelixMshOK`。

## 范围

1. `wait4`(61)：非阻塞；`(code&0xFF)<<8`；zombie reap；无子进程 → ECHILD。
2. `pipe`(22)：环形缓冲；空/满 → `-EAGAIN`；写端 refcount=0 → 读 EOF。
3. FD `refcount` + `fd_hold`；fork/dup2 共享；exit 递减（跳过 `is_console` 静态 FD）。
4. syscall 全寄存器保存/恢复（跨任务 resume）。
5. **msh**：fork/exec/wait/pipe/dup2；builtins；`|` 管道；`/bin/msh`；`-c` smoke。

## 关键架构债

协作调度：**syscall 内 `task_yield` 不会真正切换**。阻塞 I/O 必须 EAGAIN + 用户态 yield。  
曾踩坑：内核内 wait/pipe 死循环；console FD 被 free；cat 把 EAGAIN 当 EOF。

## 验收

`make smoke-linux`：`PipeOK` + `WaitOK` + `HelixMshOK` + `helixbox_smoke_done`。
