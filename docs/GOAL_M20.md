# M20 — VFS ext + 用户态补全

路线 A→B→C→D 全部完成后收尾 4 个真实差距。

## 1. BusyBox 多 applet 真实 smoke

`kernel/proc/exec.c` 的 `linux_compat_run_smoke` 在 M5 阶段只跑
`busybox echo HelixBusyBoxOK` 一个 applet。要证明 BusyBox 真正能跑（不是木偶
marker），扩展到 5-applet chain：

```
echo HelixBusyBoxOK      ← 已存在
cat /etc/welcome.txt     ← M20 subdir open
echo BB2_OK
true                     ← exit 0
echo HELIX_BB_DONE       ← final marker
```

**实现**：
- 模块级 `g_bb_idx` + `g_bb_path` + `g_bb_applets[][16]` 表
- `linux_compat_run_busybox_applets`: 取当前 applet → `task_exec_path(...)` +
  `task_set_exit_all_hook(linux_compat_run_busybox_applets)` + `task_start_user()`
- applet exit → hook fire → 自身递归 → next applet
- 末尾 → `msh_compat_run_smoke`

**Heap bump**：`HEAP_PAGES 1024 → 2048` (4→8 MiB)。每个 BusyBox ELF 重 load ≈
1.1 MiB，5 次链式 + msh + helixbox ≥ 7 MiB，原 4 MiB 不够。

## 2. FAT subdir + chmod/chown stub

**FAT subdir fix** (`kernel/fs/fat.c` `fat_getdents64`)：
- 引入 `struct fat_dir_iter { u32 clus; u8 sec; u16 off; }`
- 第一次调用时 `f->fs_priv = kmalloc(sizeof(struct fat_dir_iter))`
- 用 `it->clus` 替代 `g_fat.root_clus`；启动时 `it->clus = (fat_type==32) ? root_clus : 0`
- FAT16 root 走 fixed-region (`g_fat.root_lba + sec_i`) 保留
- sec_off 累加；`sec >= sec_per_clus` 时 `it->clus = fat_get(it->clus)`
- 0x0FFFFFF8 EOF break
- `vfs_close` 时 free `fs_priv`

**Explicit ENOSYS** (`kernel/proc/syscall.c` `case 59 execve` 后)：
```
case 21:  /* access */      ret = ERR(ENOSYS); break;
case 90:  /* chmod */       ret = ERR(ENOSYS); break;
case 91:  /* fchmod */      ret = ERR(ENOSYS); break;
case 92:  /* chown */       ret = ERR(ENOSYS); break;
case 93:  /* fchown */      ret = ERR(ENOSYS); break;
case 94:  /* lchown */      ret = ERR(ENOSYS); break;
case 132: /* utime */       ret = ERR(ENOSYS); break;
case 133: /* utimes */      ret = ERR(ENOSYS); break;
case 280: /* utimensat */   ret = ERR(ENOSYS); break;
```
不打印日志；BusyBox chmod/chown/touch 收到 ENOSYS 走标准错误路径。

## 3. FAT 深嵌套子目录验证

`scripts/mkdisk.py` 加 `--add-tree HOSTDIR::` + `--raw-fat` 选项：
- 4 级目录 `a/b/c/d/file.txt` 含 `HELIX_DEEP_OK\n`
- raw FAT volume (`out/helix-deep.raw.img`) 无 GPT 包装，mtools 可直读
- mtools 验证：`mdir -/ ::a/b/c/d` 列出 `file.txt`

## 4. msh 增强 (6 builtin)

`user/msh.c` 加 6 个 builtin + `;` statement separator + alias expansion：

**Builtins**:
| name | 行为 |
|------|------|
| `alias NAME=VALUE` | 存 alias (无 = 则查/列) |
| `unalias NAME` | 删除 alias |
| `export NAME=VALUE` | 存到 envtab (此处简化，未串入 fork()) |
| `unset NAME` | 删除 envtab |
| `test EXPR` / `[ EXPR ]` | `-f/-e/-z/-n` 单参 + `=/!=` 二元 + `-eq/-ne/-lt/-gt` 整数 |
| `type NAME` | 标 builtin vs exec |

**Parser 重构**：
- `msh_exec_line` 拆为 `msh_exec_line` + `msh_exec_pipeline` + `msh_strtok_r`
- `;` 在 `msh_exec_line` 切 statement
- `|` 在 `msh_exec_pipeline` 切 pipeline stage

**Alias expansion** (在 `msh_exec_pipeline` 早段)：
```
if alias_lookup(argv0[0]):
    tmp = alias_body
    for each tail arg in argv0[1..]:
        append space + arg to tmp
    new_argv = tokenize(tmp)
    replace argv0 with new_argv
    argc0 = new_argc
```
随后正常 dispatch：`echo HELIX_MSH_ALIAS_OK` 经 alias 展开等价于
`echo HELIX_MSH_ALIAS_OK`，bi_echo 打印。

**bi_test bug fix**：`test -f FILE` argc=3 (test/-f/FILE)，原代码先检查
`argc==3` 的 binary `=` 操作符，误报 "need binary expr"。修正：先扫描
`argv[1][0]=='-'` 触发 unary 分支。

## 5. Smoke 验收

```
make smoke-linux
```

期望串口含：
- `BusyBox chain done (5 applets)` (kernel 打印 chain 结束)
- `HELIX_MSH_ALIAS_OK` (`alias x=echo; x HELIX_MSH_ALIAS_OK`)
- `HELIX_MSH_EXPORT_OK` (`export A=42; echo HELIX_MSH_EXPORT_OK`)
- `HELIX_MSH_TEST_OK` (`test -f /hello.txt; echo HELIX_MSH_TEST_OK`)
- `HELIX_MSH_DONE` (`echo HELIX_MSH_DONE | cat`)
- 已有 `HelixBusyBoxOK` / `HELIX_BB_DONE` 不回归