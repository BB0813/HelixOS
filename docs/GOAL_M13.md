# Goal: HelixOS M13 — 信号最小集（历史）

> **状态：已完成（2026-08-01）** — **`HelixSigOK`**。

## 范围

1. per-task `sig_pending` / `sig_blocked` / `sighand[HELIX_NSIG]`  
2. `kill`(62) / `rt_sigaction`(13) / `rt_sigprocmask`(14)  
3. **SIGCHLD**：子 exit → 父 pending；默认忽略  
4. **SIGINT**：COM1 Ctrl+C → 当前 task；默认终止  
5. **SIGTERM/SIGKILL**：default terminate；SIGKILL 不可屏蔽  
6. helixbox：`fork` + `kill(child, SIGTERM)` + `wait4` → **`HelixSigOK`**  
7. 投递时机：syscall 返回用户前 `signal_deliver_current`

## 约束 / 非目标

- 协作调度：中断/路径只置位，不抢占跑 handler  
- 用户态自定义 handler 帧未做（handler 非 DFL/IGN 时 drop）  
- 无进程组 / 会话 / sigaltstack / 实时信号队列  

## 验收

```bash
make smoke-linux
# serial.log: HelixSigOK + HelixCwdOK + HelixMshOK + helixbox_smoke_done
```

## 实现要点

- `include/helix/signal.h` · `kernel/proc/signal.c`  
- `task` 字段 + `task_exit_current` → `signal_on_exit`  
- `cons_read` Ctrl+C → `signal_send(SIGINT)`  
- `syscall_entry_c` 返回前 `signal_deliver_current`  
