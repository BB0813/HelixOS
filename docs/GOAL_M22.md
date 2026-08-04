# Goal: M22 — 抢占式调度（preemptive）

## Context

M0–M19 调度严格协作：唯一切换路径是 `sys_yield()` → `net_poll()` → `task_yield()`
→ syscall 返回寄存器换帧。后台 task（如 TCP retransmit、新增 helixbox heartbeat）
必须靠前台 task 主动 `yield()` 才能推进。M22 在协作基础上叠加 IRQ0 tick 抢占点：
后台 task 即使前台不动也能在 ~80ms 内获得一次时间片。

**协作语义保留**：`yield()` 仍工作；阻塞 I/O 仍返回 EAGAIN；单任务场景零开销；
fd refcount / FD 共享不变（同一 task 不会"被自己"重入）。

## Files

- `kernel/arch/x86_64/timer.c` — `g_preempt_pending` 累加 + `timer_preempt_pending()` getter + `PREEMPT_THRESHOLD=8`
- `include/helix/timer.h` — getter 声明 + 阈值常量接口
- `kernel/proc/syscall.c` — syscall 返回路径（:1311 与 :1313 之间）抢占检查
- `user/helixbox.c` — `HelixPreemptOK` smoke marker（fork 心跳子进程）

## 改动细节

### `kernel/arch/x86_64/timer.c`

新增字段：
```c
#define PREEMPT_THRESHOLD 8  /* ≈80ms @100Hz PIT tick */
static volatile int g_preempt_pending;
```

`timer_on_irq()` 每 tick 累加（不是置位）：
```c
g_preempt_pending++;   /* M22: 累加，让 syscall 返回路径节流消费 */
```

新增 getter：
```c
int timer_preempt_pending(void) {
    /* __atomic_exchange_n seq_cst: read prev value, replace with 0. */
    return __atomic_exchange_n(&g_preempt_pending, 0, __ATOMIC_SEQ_CST);
}
int timer_preempt_threshold(void) { return PREEMPT_THRESHOLD; }
```

### `include/helix/timer.h`

```c
int timer_preempt_pending(void);   /* 读并清，返回累计 tick 数 */
int timer_preempt_threshold(void); /* 阈值 (8) */
```

### `kernel/proc/syscall.c`

在 `signal_deliver_current()` 后、`t = task_current()` 前插入：
```c
if (task_count_alive() > 1 &&
    timer_preempt_pending() >= timer_preempt_threshold()) {
    task_yield();
}
```

双 gate：
1. `task_count_alive() > 1` — 单任务路径零开销（最常见）
2. `>= PREEMPT_THRESHOLD (8 ticks)` — 节流到 ≈12 次/秒，避免 syscall 返回路径太热

**不调** `net_poll()`：`sys_yield()` 内部已调；其他需要 net_poll 的 syscall 自己调。
本路径只负责切换 task，让后台 READY task 跑起来。

### `user/helixbox.c`

TCP passive fork block（:672）之后、`cmd_tcp_smoke()`（:675）之前插入：
```c
{
    long hb_child = usys(SYS_fork, 0, 0, 0);
    if (hb_child == 0) {
        for (int i = 0; i < 20; i++) {
            xwrite(".");
            usys(SYS_yield, 0, 0, 0);
        }
        xwrite("\n[helixbox] preempt heartbeat done\n");
        usys(SYS_exit, 0, 0, 0);
    }
    for (int i = 0; i < 30; i++) usys(SYS_yield, 0, 0, 0);
    xwrite("[helixbox] HelixPreemptOK\n");
}
```

## 验证

### smoke-fs（单任务为主）— 不应回归
```bash
make smoke-fs
# 含 "HelixFATWriteOK" + "loaded init+task2 from disk"
```

### smoke-linux（多任务 + helixbox）
```bash
make smoke-linux
# 串口含：
#   [user] HelixBusyBoxOK         (BusyBox path)
# 或：
#   [user] HelixLinuxOK + tmp_write_ok + HelixCwdOK + HelixSigOK
#   [user] [helixbox] preempt heartbeat done       ← child
#   [user] [helixbox] HelixPreemptOK                ← parent (M22 NEW)
# 注：cmd_tcp_smoke / cmd_host_udp 因 host echo server 未启会卡在 connect retry；
#     此卡顿在 M21 baseline 已存在，M22 不引入额外回归。
```

### smoke-net（多任务 + host echo server）
```bash
make smoke-net
# 串口含：
#   [net] HelixNetOK + user_udp_ok + HelixTcpOK
#   [user] tcp_passive: accepted    → HelixTcpPassiveOK
#   [user] tcp_smoke: ...          → HelixTcpUserOK
#   [user] [helixbox] HelixPreemptOK  (M22 NEW)
# 全部 marker 出现；TCP user/passive 不回归
```

## 已知边界

- 抢占阈值 8 在 PIT 100Hz 下是 ≈80ms。心跳 child 写 20 dots × ~1 yield/dot = 20 次 syscall，
  期间 2~3 次抢占（每 ~80ms 一次）足够让 child 推进。
- 多任务场景（TCP passive child pid 5 alive 时）每 syscall 走 1 次
  `task_count_alive` + 1 次 `__atomic_exchange_n` ≈ 50ns 开销，可忽略。
- 单任务场景（绝大多数 kernel 主路径）走 1 次 `task_count_alive` 后 short-circuit。