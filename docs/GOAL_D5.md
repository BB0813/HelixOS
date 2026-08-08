# Goal: D5 — getrandom 真熵 + heap full coalesce + execve argv 审查

## Context

issue #1 CRITICAL #4（getrandom deterministic）+ 部分 MAJOR（heap 外碎片 /
execve argv 疑云）。不阻塞功能但影响可信度。

## Files

| File | Change |
|------|--------|
| `kernel/proc/syscall.c` | `sys_getrandom`：CPUID leaf 1 ECX bit 30 检测 `RDRAND`，可用走硬件（10 次重试 + RDSEED 语义）；不可用 fallback Galois LFSR (`lfsr_next_byte`) over TSC |
| `kernel/mm/heap.c` | `kfree` full coalesce：线性 free-list scan 找 next_phys 吸收，再找 prev_phys（end == freed start）吸收 freed block |
| `user/helixbox.c` | `HelixGetrandomOK`（verify buffer 非全 0 + non-pattern）+ `HelixMallocOK`（连续 kfree 相邻块 → kmalloc 大块一次成功） |

## getrandom

```c
static int has_rdrand(void);              /* CPUID.1:ECX[30] */
static u8  lfsr_next_byte(void);          /* Galois LFSR fallback */
i64 sys_getrandom(u64 buf, u64 buflen, u64 flags);  /* NR 318 */
```

明确注释：headless QEMU (TCG) 无 RDRAND 仍 deterministic-ish；比纯
`timer_ticks + i*37` 强。

## heap full coalesce

- kfree 时线性 scan free list：找到 `next_phys == freed_block_start` 的 free 块
  吸收之；再找 prev_phys（其 end == freed block start）吸收 freed block。
- 合并后把结果 size 写回结果块头，**不再**把 freed block 加回 free list。
- 验证：连续 kfree 三个相邻块 → 再 kmalloc 大块应一次成功。

## execve argv 审查结论

**无 leak**：argv 被 push 到 user stack（而非 kernel heap）；失败路径无 kernel 侧
副本需 kfree。D4 审查确认，未改动代码。

## 验收

- `make smoke-linux` 含 `HelixGetrandomOK` + `HelixMallocOK`，无 FAIL
- `make smoke-fs` EXIT=0

## 串口 marker

| Marker | 含义 |
|--------|------|
| **HelixGetrandomOK** | getrandom 返回非全 0 非 pattern buffer |
| **HelixMallocOK** | 相邻 kfree 后大块 kmalloc 一次成功（coalesce 生效） |
