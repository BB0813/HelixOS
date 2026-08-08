# Goal: D4 — 内存安全 trio (munmap / mprotect / vmm_unmap)

## Context

Sakura AI 扫描 ([issue #1](https://github.com/BB0813/HelixOS/issues/1)) CRITICAL
#1/#2/#3：`sys_munmap` / `sys_mprotect` / `vmm_unmap_user_range` 都是空 stub。
用户 munmap 后页面仍占用（leak + busybox mmap loop 失控）；mprotect 返回成功但
实际不变（破坏 W^X 语义 + 依赖 PROT_NONE 的应用 silent fail）。

## 关键约束：共享单 PML4

HelixOS **所有 task 共享一个 PML4**。释放任何 phys page 会同时从所有 peer task
移除 → 立刻 #PF。这是真实 unmap/prot 的硬阻塞，D4 先落地 infra + no-op stubs，
真实修复（per-task PML4 + COW）延后到 M25+。

## Files

| File | Change |
|------|--------|
| `include/helix/mm_layout.h` | USER_BASE / USER_STACK_TOP / USER_LOW window 等地址常量集中（D6 落地，D4 依赖） |
| `kernel/arch/x86_64/paging.c` | `paging_unmap_4k`（4 级 walk + `pmm_free_page` user leaf + `invlpg` + 空中间表回收）+ `paging_set_prot_range`（toggle PTE_W）+ `table_count_present` |
| `kernel/mm/vmm.c` | `vmm_unmap_user_range(virt, len)` + `vmm_set_prot(virt, len, prot)` — 暂为 no-op（D4 TODO: 需 per-task PML4） |
| `kernel/proc/syscall.c` | `sys_munmap` / `sys_mprotect` — 暂为 no-op（D4 TODO） |
| `kernel/proc/task.c` | task_exit 注释文档化共享 PML4 限制 |
| `user/helixbox.c` | `HelixMunmapOK` + `HelixMprotectOK` markers（验证 mmap 路径不回归） |

## paging.c 实现要点

```c
/* 4 级页表 walk 单页 user leaf PTE → pmm_free_page + invlpg，
 * 空掉的 intermediate 表级联 free。 */
int paging_unmap_4k(u64 virt);
/* walk 范围内每页 PTE，按 writable 设/清 PTE_W 位，invlpg 刷 TLB。 */
int paging_set_prot_range(u64 virt, u64 len, int writable);
/* 统计给定 4K 表内 present 项数（供空表回收决策）。 */
static int table_count_present(const u64 *tab);
```

paging 函数已实现并编译验证；**不**通过 sys_munmap/sys_mprotect 调用（暂为
no-op），因为共享 PML4 下释放会破坏 peer task。

## 验收

- `make smoke-linux` 全 marker pass（含 HelixMunmapOK + HelixMprotectOK）
- `make smoke-fs` EXIT=0（FAT 不回归）
- `[syscall] ENOSYS` 刷屏为空

## 已知限制

- 共享 PML4 → sys_munmap/sys_mprotect 是 no-op（返回 0 不释放）
- D4.2 (per-task PML4 + COW) 是 M25+ 级重构，阻塞 issue #1 CRITICAL #1/#2/#3
  真实修复

## 串口 marker

| Marker | 含义 |
|--------|------|
| **HelixMunmapOK** | sys_munmap 路径不回归 |
| **HelixMprotectOK** | sys_mprotect 路径不回归 |
