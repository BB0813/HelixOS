# Goal: D6 — UI/UX 清理 + 文档

## Context

路线 D 收尾：集中散落的地址常量、集中 stdio 初始化、补 D 系列文档。三个 MINOR
项的代码部分全部落地；color prefix 项延后。

## Files

| File | Change |
|------|--------|
| `include/helix/mm_layout.h` | 集中 USER_BASE / USER_STACK_TOP / USER_STACK_SIZE / USER_LOW_MIN / USER_LOW_MAX / INTERP_BASE / INTERP_MAX（从 syscall.h 等的 inline 定义迁出） |
| `kernel/proc/syscall.c` | `fd_init_task_stdio()` 从每个 handler 重复调改为 `syscall_entry_c` 入口一次 |
| `docs/GOAL_D4.md` `docs/GOAL_D5.md` `docs/GOAL_D6.md` | 新建 |
| `docs/ROADMAP.md` `docs/ARCHITECTURE.md` `docs/SYSCALLS.md` `README.md` | 加 D4–D6 节 |

## mm_layout.h

```c
#define USER_BASE       0x0000000040000000ULL
#define USER_LOW_MIN    0x0000000000400000ULL   /* classic ET_EXEC min */
#define USER_STACK_TOP  ...  /* 每任务 stack 顶 */
#define USER_LOW_MAX    ...
#define INTERP_BASE     ...  /* ld-helix / ld-musl 加载窗 */
#define INTERP_MAX      ...
```

单一来源，删 syscall.h 内重复 inline 定义。

## fd_init_task_stdio 集中

- 之前：`sys_read`/`sys_write`/`sys_poll`/`sys_fsync` 等 handler 各自调用
  `fd_init_task_stdio()`（每次重建 task 的 0/1/2 FD 表）。
- 现在：`syscall_entry_c` 入口处调用一次，所有 handler 依赖已初始化状态。

## 延后项

- **kprintf ANSI color prefix**：`[fat]`/`[net]`/`[tcp]` log 加颜色，headless 下
  `isatty(serial)` 永远 false → 不变色。低价值，延后。

## 验收

- `make` EXIT=0（kernel + user 编译干净）
- `make smoke-linux` EXIT=0（全部 marker pass）
- `make smoke-fs` EXIT=0
