# Goal: M21 — FAT32 完善

## Context

M4 引入 FAT16 写路径，但 fat.c 已隐式支持 FAT32（mount、簇分配、FAT 表更新、目录遍历全部有 `fat_type == 32` 分支）。M21 主要补两个东西：

1. **`fat_selftest_write` 二次启动 cleanup bug**：旧实现只在 FAT16 删除 stale `HELIXW.TXT`；FAT32 不删，导致二次启动时 stale 文件残留（cluster 链被旧 dirent 占着），新 selftest 写入可能落到 stale cluster 上。
2. **FAT32 大盘镜像验证**：ESP ≥ 64 MiB 时自动切到 FAT32（mkdisk.py），完整 boot path 在真实 FAT32 上跑通。

## Files

- `kernel/fs/fat.c` — `fat_free_chain()` + `root_unlink_and_free()`；`fat_selftest_write` 走新 helper
- `scripts/mkdisk.py` — `build_fat32_volume()` + `main` 根据 ESP size 选择 FAT16/FAT32

## 改动细节

### `kernel/fs/fat.c`

新增两个 helper（紧跟 `root_update_dirent` 之后）：

```c
/* 释放 cluster chain。FAT entries → 0，遇到 EOF 停止 */
static void fat_free_chain(u32 c);

/* 找到 root 中匹配 8.3 name 的 entry，mark 0xE5 + free cluster chain。
 * 同时处理 FAT16 固定 root region 和 FAT32 root cluster chain */
static int root_unlink_and_free(const char name83[11]);
```

`fat_selftest_write` 把原来的 25 行 FAT16-only cleanup block 替换成 4 行：
```c
{
    char w[11];
    encode_83_upper("HELIXW.TXT", w);
    root_unlink_and_free(w);
}
```

### `scripts/mkdisk.py`

新增 `build_fat32_volume(size_bytes, files)`，模仿 `build_fat16_volume` 但：
- `reserved = 32`（FAT32 需要 FSInfo + backup boot sector 空间）
- `root_entries = 0`（BPB 标志）
- 4-byte FAT entries，`fat_set` 写 4 bytes
- 簇链 EOF mark `0x0FFFFFF8`
- 目录项 `e[20:22]` 高 16 位 cluster
- BPB：root_clus、fsinfo_sector (1)、bkbootsec (6)、fatsz32、tot32
- FSInfo sector（sector 1）签名 `0x41615252` / `0x61417272`

`main()` 根据 ESP size 选择：
```python
if esp_size >= 64 * 1024 * 1024:
    fat = build_fat32_volume(esp_size, files)
else:
    fat = build_fat16_volume(esp_size, files)
```

## 已知问题

`build_fat32_volume` 的 root dir materialization 在子目录递归 + dirent 链扩展路径上有 bug（nested dirs 部分 dirent 没正确写入 root cluster chain）。
本里程碑**只**用 FAT32 跑 flat root（boot EFI + 顶层 `bin/`、`lib/`、`hello.txt`），子目录用 mtools 注入 + 手动构建 GPT 验证 FAT32 mount/write。
后续若要自动化 FAT32 镜像，需重写 `build_fat32_volume` 的 root chain 扩展逻辑。

## 验证

### FAT16（默认 32 MiB ESP）— 不回归
```bash
make smoke-fs
# 串口含 "mounted FAT16" + "selftest OK (FAT16)" + HelixFATWriteOK
```

### FAT32（64 MiB ESP）— 大盘镜像
通过 mformat + mcopy 准备 FAT32 镜像：
```bash
dd if=/dev/zero of=esp.img bs=1M count=64
mformat -i esp.img -F -v HELIXOS ::
mcopy -i esp.img -sop esp/EFI ::       # boot loader
mcopy -i esp.img -sop build/user/* ::bin
mcopy -i esp.img -sop build/user/ld-helix.so ::lib
```
用 `mkdisk.py build_gpt_disk` 包装成 GPT 镜像，挂 QEMU。

### 串口验收
```
[fat] mounted FAT32 spc=1 fatsz=1009 root_ent=0 data_lba=2050 clusters=129022
[fat] write/mkdir enabled on root (FAT32)
[fat] selftest resolved clus=3 size=16
[fat] selftest OK (FAT32): HelixFATWriteOK
[fs] HelixFATWriteOK
```

### 二次启动 stale-cleanup 验证
直接再 boot 一次，**不**重新构造镜像。检查：
- selftest 仍 OK（size=16 匹配）
- HELIXW.TXT dirent 仍在 root（cluster 3，新分配）
- 没有 cluster 泄漏（旧 cluster 3 的 chain 已 free）