# Goal: D4 — 内存安全 trio (munmap / mprotect / vmm_unmap)

## Context

Sakura AI 扫描 ([issue #1](https://github.com/BB0813/HelixOS/issues/1)) CRITICAL
#1/#2/#3：`sys_munmap` / `sys_mprotect` / `vmm_unmap_user_range` 都是空 stub。
用户 munmap 后页面仍占用（leak + busybox mmap loop 失控）；mprotect 返回成功但
实际不变（破坏 W^X 语义 + 依赖 PROT_NONE 的应用 silent fail）。

**D4.2 (M25) 已落地真实修复**：per-task PML4 激活后三个入口都走真实实现。

## 关键约束：共享单 PML4 → D4.2 解除

原：HelixOS **所有 task 共享一个 PML4**，释放任何 phys page 会同时从所有 peer
task 移除 → 立刻 #PF。这是真实 unmap/prot 的硬阻塞，D4 先落地 infra + no-op
stubs。

D4.2 (M25)：每个 task 拥有自己的 pml4（`task_create` 克隆 kernel 模板），每次
context switch `task_activate()` 加载 CR3。kernel 2MiB identity leaves 按 PTE-value
共享（per-task tables、shared phys），user 页与 per-task 表完全隔离。真实
unmap/prot/execve 均只影响当前 task 的地址空间。

## Files

| File | Change |
|------|--------|
| `include/helix/mm_layout.h` | USER_BASE / USER_STACK_TOP / USER_LOW window 等地址常量集中（D6 落地，D4 依赖） |
| `kernel/arch/x86_64/paging.c` | `paging_unmap_4k`（4 级 walk + `pmm_page_deref` user leaf + `invlpg` + 空中间表回收）+ `paging_set_prot_range`（toggle PTE_W）+ `table_count_present` + `g_kernel_pml4` + `paging_set_pml4` |
| `kernel/mm/pmm.c` | per-page refcount：`pmm_page_own/share/deref/refcount`（共享页不被 peer 释放） |
| `kernel/mm/vmm.c` | `vmm_unmap_user_range(virt, len)` — 真实（逐页 `paging_unmap_4k`）; `vmm_set_prot(virt, len, prot)` — 真实（`paging_set_prot_range`）; `vmm_clone_kernel_pml4`; `vmm_destroy_address_space` |
| `kernel/proc/syscall.c` | `sys_munmap` / `sys_mprotect` — 真实（校验对齐 + 范围窗口后调用 vmm 实现）; `sys_execve` 换新地址空间 |
| `kernel/proc/task.c` | `task_create` 分配 per-task pml4; `task_activate` 每次 context switch 加载 CR3; fork 存子进程 pml4; exit 先切 pml4 再 destroy |
| `kernel/proc/exec.c` + `kernel/proc/elf.c` | ELF + stack 加载进 task 自己的 pml4; `elf_load_bitmaps_reset` |
| `user/helixbox.c` | `HelixMunmapOK`（munmap 后 MAP_FIXED 同址重 mmap 验证 PTE 真清）+ `HelixMprotectOK`（写前后比对 PTE_W） |

## paging.c 实现要点

```c
/* 4 级页表 walk 单页 user leaf PTE → pmm_page_deref + invlpg，
 * 空掉的 intermediate 表级联 free。 */
int paging_unmap_4k(u64 virt);
/* walk 范围内每页 PTE，按 writable 设/清 PTE_W 位，invlpg 刷 TLB。 */
int paging_set_prot_range(u64 virt, u64 len, int writable);
/* 统计给定 4K 表内 present 项数（供空表回收决策）。 */
static int table_count_present(const u64 *tab);
```

D4.2 后 `paging_unmap_4k` / `paging_set_prot_range` 经 `vmm_unmap_user_range` /
`vmm_set_prot` 由 sys_munmap / sys_mprotect 真实调用 — 只 walk 当前 task 的
per-task PML4，不破坏 peer。

## 验收

- `make smoke-linux` 全 marker pass（含真实 HelixMunmapOK + HelixMprotectOK + HelixPreemptOK）
- `make smoke-fs` EXIT=0（FAT 不回归）
- `make smoke-net` EXIT=0（fork-heavy TCP 不回归）
- `[syscall] ENOSYS` 刷屏为空

## 已知限制

- fork 仍 eager-copy（COW 延后 M26+；refcount 已就绪）
- PROT_NONE 保持 present+readable（无 fault-recovery handler，避免 #PF panic）
- `make smoke-shell` pre-existing 时序失败：shell 仅在 kernel idle loop 处理命令，
  userland smoke 链跑完前不会响应（与 D4.2 无关，stash 验证 baseline 同样失败）

## 串口 marker

| Marker | 含义 |
|--------|------|
| **HelixMunmapOK** | sys_munmap 路径不回归 |
| **HelixMprotectOK** | sys_mprotect 路径不回归 |
