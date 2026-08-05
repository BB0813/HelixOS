# Goal: M24 — POSIX file ops 收尾 (poll/fsync/unlink/rmdir/rename)

## Context

M23 收尾 PS/2 鼠标后，helixbox 用户态已基本能跑（busybox + msh + helixbox + mouse
+ kb + cwd + signals），但还有 4 个 Linux ABI 差距需要收：

1. **未实现的 poll(2) / ppoll(2) syscall** — BusyBox 不少 applet（`cat` 用
   ppoll 等 `EINTR`；`tail -f` 用 poll）会触发 `ENOSYS` 刷屏。
2. **unlink / rmdir / rename(2) 未实现** — helixbox smoke 已经能 `cat /hello.txt`
   但无法创建/删除文件，无法 mkdir/rmdir，BusyBox `rm`/`mv` 一调就 -ENOSYS。
3. **fsync(2) / fdatasync(2) 未实现** — stdio 库的 `_IO_finish_write` 会触发；
   即使绕过 libc，POSIX 严格要求。
4. **silent ENOSYS** — M23 前未实现 syscall 走 default 时 `kprintf` 刷屏；M24
   把 default 改成静默 return `ERR(ENOSYS)`，日志清洁。

M24 三个 commit 全部落实 + smoke 验证 + 文档。

## Files

| File | Change |
|------|--------|
| `include/helix/vfs.h` | `struct helix_pollfd` + POLL defines + `poll/unlink/rmdir/rename/fsync` ops fields + `vfs_poll_one` |
| `include/helix/syscall.h` | (already had `SYS_poll=7`, `SYS_ppoll=271` — Linux NRs) |
| `kernel/proc/syscall.c` | `sys_poll` / `sys_ppoll` / `sys_unlink` / `sys_rmdir` / `sys_rename` / `sys_fsync` / `sys_fdatasync` + dispatch cases (74/75/82/84/87) + silent ENOSYS default |
| `kernel/fs/vfs.c` | `vfs_unlink/rmdir/rename` dispatchers (FAT + ramfs); `vfs_poll_one` default handler |
| `kernel/fs/fat.c` | `fat_resolve_parent` + `dir_unlink_at` (0xE5 + cluster free) + `dir_rename_at` + `dir_is_empty` + `fat_unlink_path` / `fat_rmdir_path` / `fat_rename_path` |
| `kernel/fs/ramfs.c` | `node_release` + `node_child_count` + `ramfs_unlink_op` / `ramfs_rmdir_op` / `ramfs_rename_op` + wire into `g_ramfs_ops` |
| `user/helixbox.c` | M24 smoke blocks: `HelixPollOK` (poll(0, [fda=3 stdin], 1, 0)) + `HelixUnlinkOK` (create /tmp/u.txt, unlink, mkdir, rmdir, rename) + `HelixFsyncOK` (write + fsync + fdatasync + bad-fd EBADF + O_TRUNC truncates) |
| `docs/ROADMAP.md` | M24 节 |
| `docs/ARCHITECTURE.md` | M24 子节 |
| `docs/SYSCALLS.md` | poll/ppoll/unlink/rmdir/rename/fsync/fdatasync 表格行 |
| `docs/GOAL_M24.md` | 本文件 |

## 改动细节

### 1. `include/helix/vfs.h`

新增 Linux poll(2) 子集常量 + `helix_pollfd`:
```c
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct helix_pollfd { int fd; short events; short revents; };  /* 8 bytes */
```

`struct vfs_ops` 在原 9 字段上加 `poll / unlink / rmdir / rename / fsync`，变成 14 字段
(112 字节):
```c
struct vfs_ops {
    int  (*open)(const char *, int, struct vfs_file **);
    int  (*read)(struct vfs_file *, void *, u64, u64 *);
    int  (*write)(struct vfs_file *, const void *, u64, u64 *);
    int  (*close)(struct vfs_file *);
    int  (*readdir_root)(void (*)(const char *, u64, void *), void *);
    long (*getdents64)(struct vfs_file *, void *, u64);
    long (*fstat)(struct vfs_file *, void *);
    int  (*mkdir)(const char *, int);
    int  (*poll)(struct vfs_file *);
    int  (*unlink)(const char *);
    int  (*rmdir)(const char *);
    int  (*rename)(const char *, const char *);
    int  (*fsync)(struct vfs_file *);
};
```

**重要**: 任何 `struct vfs_ops` 字面量都必须重新编译 (fat.c, ramfs.c, pipe.c, vfs.c)。
未重新编译的对象会产生 struct 大小 mismatch —— 这是 M24-1 早期 HelixPollFAIL 的根因。

### 2. `kernel/proc/syscall.c`

#### 2a. `sys_poll` / `sys_ppoll`

```c
static i64 sys_poll(u64 fds_ptr, u64 nfds, u64 timeout_ms) {
    if (nfds == 0) return 0;
    if (!user_ptr_ok((const void *)(uintptr_t)fds_ptr,
                     nfds * sizeof(struct helix_pollfd)))
        return ERR(EFAULT);
    struct helix_pollfd *pf = (struct helix_pollfd *)(uintptr_t)fds_ptr;
    (void)timeout_ms;  /* 不阻塞 */
    int ready = 0;
    for (u64 i = 0; i < nfds; i++) {
        if (!user_ptr_ok(&pf[i].revents, sizeof(short)))
            return ERR(EFAULT);
        short ev = pf[i].events;
        short rev = 0;
        if (pf[i].fd < 0) {
            rev = 0;
        } else {
            fd_init_task_stdio();
            struct vfs_file *f = fd_get(pf[i].fd);
            if (!f) rev = (short)POLLNVAL;
            else rev = (short)vfs_poll_one(f, ev);
        }
        pf[i].revents = rev;
        if (rev) ready++;
    }
    return (i64)ready;
}

static i64 sys_ppoll(u64 fds, u64 nfds, u64 ts_ptr, u64 sigmask_ptr, u64 sigmask_sz) {
    (void)ts_ptr; (void)sigmask_ptr; (void)sigmask_sz;
    return sys_poll(fds, nfds, 0);
}
```

`vfs_poll_one`: 优先调 `f->ops->poll`; 否则按文件类型 default (stdin → POLLIN, stdout
→ POLLOUT, dir → POLLIN, regular → POLLIN|POLLOUT). 返回值 `& events`.

#### 2b. `sys_unlink` / `sys_rmdir` / `sys_rename`

```c
static i64 sys_unlink(u64 path) {
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (vfs_unlink(abs) != 0)
        return ERR(ENOENT);
    return 0;
}

static i64 sys_rmdir(u64 path) {
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (vfs_rmdir(abs) != 0)
        return ERR(EACCES);
    return 0;
}

static i64 sys_rename(u64 oldp, u64 newp) {
    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    if (resolve_user_path(oldp, old_abs, sizeof(old_abs)) != 0) return ERR(EFAULT);
    if (resolve_user_path(newp, new_abs, sizeof(new_abs)) != 0) return ERR(EFAULT);
    if (vfs_rename(old_abs, new_abs) != 0) return ERR(EACCES);
    return 0;
}
```

#### 2c. `sys_fsync` / `sys_fdatasync`

```c
static i64 sys_fsync(u64 fd) {
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f) return ERR(EBADF);
    if (f->is_console) return 0;
    if (f->ops && f->ops->fsync)
        return f->ops->fsync(f) == 0 ? 0 : ERR(EIO);
    return 0;
}
static i64 sys_fdatasync(u64 fd) { return sys_fsync(fd); }
```

FAT `write_sector` 已同步走 AHCI，ramfs 在内存 — `fsync` 实际是 no-op；保留 op 字段以备
未来 buffered fs。

#### 2d. silent ENOSYS default

```c
default:
    /* M24: silent ENOSYS — no kprintf spam for unimplemented syscalls. */
    ret = ERR(ENOSYS);
    break;
```

init message 改为 `"init syscall dispatch (M24 poll/ppoll + silent ENOSYS)"`.

#### 2e. dispatch cases

```c
case SYS_poll:            ret = sys_poll(f->a0, f->a1, f->a2); break;
case SYS_ppoll:           ret = sys_ppoll(f->a0, f->a1, f->a2, f->a3, f->a4); break;
case 82:  /* rename */    ret = sys_rename(f->a0, f->a1); break;
case 84:  /* rmdir */     ret = sys_rmdir(f->a0); break;
case 87:  /* unlink */    ret = sys_unlink(f->a0); break;
case 74:  /* fsync */     ret = sys_fsync(f->a0); break;
case 75:  /* fdatasync */ ret = sys_fdatasync(f->a0); break;
```

### 3. `kernel/fs/vfs.c`

```c
extern int fat_unlink_path(const char *path);
extern int fat_rmdir_path(const char *path);
extern int fat_rename_path(const char *oldp, const char *newp);

int vfs_unlink(const char *path) {
    if (!path) return -1;
    if (is_tmp_path(path)) {
        if (!g_tmp_ops || !g_tmp_ops->unlink) return -1;
        return g_tmp_ops->unlink(path);
    }
    return fat_unlink_path(path);
}
int vfs_rmdir(const char *path) { /* same shape */ }
int vfs_rename(const char *o, const char *n) {
    if (!o || !n) return -1;
    if (is_tmp_path(o) && is_tmp_path(n)) {
        if (!g_tmp_ops || !g_tmp_ops->rename) return -1;
        return g_tmp_ops->rename(o, n);
    }
    if (is_tmp_path(o) || is_tmp_path(n)) return -1;
    return fat_rename_path(o, n);
}

int vfs_poll_one(struct vfs_file *f, short events) {
    if (!f) return POLLNVAL;
    if (f->ops && f->ops->poll) {
        int mask = f->ops->poll(f);
        if (mask < 0) return POLLERR;
        return mask & (int)events;
    }
    int mask = 0;
    if (f->is_console == 2)        mask |= POLLIN;
    else if (f->is_console == 1)   mask |= POLLOUT;
    else if (f->is_dir)            mask |= POLLIN;
    else if (f->is_socket == 2)    mask |= POLLIN | POLLOUT;
    else {                         mask |= POLLIN | POLLOUT;
                                   if (f->size == 0) mask &= ~POLLIN;
    }
    return mask & (int)events;
}
```

### 4. `kernel/fs/fat.c`

#### 4a. `fat_resolve_parent`

```c
static int fat_resolve_parent(const char *path, u32 *out_parent, char out_name83[11]) {
    while (*path == '/') path++;
    if (!*path) return -1;
    u32 dir_clus = (g_fat.fat_type == 32) ? g_fat.root_clus : 0;
    char comp[64];
    for (;;) {
        int n = 0;
        while (*path && *path != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *path++;
        comp[n] = 0;
        while (*path == '/') path++;
        char w[11];
        encode_83_upper(comp, w);
        if (!*path) {
            memcpy(out_name83, w, 11);
            *out_parent = dir_clus;
            return 0;
        }
        u32 cl = 0, sz = 0;
        u8 attr = 0;
        if (find_in_dir(dir_clus, w, &cl, &sz, &attr) != 0) return -1;
        if (!(attr & 0x10)) return -1;
        dir_clus = cl;
    }
}
```

#### 4b. `dir_unlink_at`

Mark entry `e[0] = 0xE5`, write back sector, free cluster chain via
`fat_free_chain(first_cluster)`. 处理 FAT16 root region (`parent_clus == 0` && 非
FAT32) 用 `g_fat.root_lba + sec_i`; FAT32 subdirs 走 cluster chain (FAT32 root 或
subdir).

#### 4c. `dir_rename_at`

In-place `memcpy(e, new_name83, 11)`. 同目录限定 (`fat_rename_path` 检查 `old_parent
== new_parent`).

#### 4d. `dir_is_empty`

跳过 `0` / `0xE5` / LFN (`attr == 0x0F`) / `.` / `..`. 见到任何有效 entry 返回 0.

#### 4e. high-level wrappers

```c
int fat_unlink_path(const char *path) { /* check fat_writable_type → resolve_parent → dir_unlink_at */ }
int fat_rmdir_path(const char *path) {
    /* resolve_path → must be dir AND empty → dir_unlink_at */
}
int fat_rename_path(const char *o, const char *n) {
    /* resolve both parents → same-dir check → dir_rename_at */
}
```

### 5. `kernel/fs/ramfs.c`

#### 5a. `node_release(idx)`: `kfree(data)` + `memset` slot 0

#### 5b. `node_child_count(idx)`: linear scan

#### 5c. `ramfs_unlink_op(path)`, `ramfs_rmdir_op(path)`, `ramfs_rename_op(o, n)`

```c
static int ramfs_unlink_op(const char *path) {
    const char *rel; if (strip_tmp_prefix(path, &rel) != 0) return -1;
    if (!*rel || (rel[0]=='.' && rel[1]==0)) return -1;
    int parent; char leaf[RAMFS_MAX_NAME];
    if (resolve(rel, 1, &parent, leaf, sizeof(leaf)) != 0) return -1;
    int idx = find_child(parent, leaf); if (idx < 0) return -1;
    node_release(idx); return 0;
}
static int ramfs_rmdir_op(const char *path) { /* same + is_dir check + empty check */ }
static int ramfs_rename_op(const char *o, const char *n) {
    /* resolve both → same parent → no target exists → memcpy new name */
}
```

接到 `g_ramfs_ops` 字段 `.unlink / .rmdir / .rename`.

### 6. `user/helixbox.c`

#### 6a. defines
```c
#define SYS_unlink      87
#define SYS_fsync       74
#define SYS_fdatasync   75
```

#### 6b. HelixPollOK (insert after HelixCwdOK)
```c
struct helix_pollfd pfd[1] = { { .fd = 0, .events = POLLIN } };
long pr = usys(SYS_poll, (long)pfd, 1, 0);
if (pr >= 0) xwrite("HelixPollOK\n");
```

#### 6c. HelixUnlinkOK (after cwd block)
```c
/* 1. create /tmp/u.txt; 2. unlink; 3. open-after-unlink must fail;
 * 4. rmdir-on-file must fail; 5. mkdir + rmdir empty;
 * 6. create /tmp/a.txt → rename /tmp/b.txt → open a (fail), open b (ok) */
```

#### 6d. HelixFsyncOK (after unlink block)
```c
/* 1. open /tmp/f.txt O_WRONLY|O_CREAT; write; fsync; fdatasync; close — both must return 0.
 * 2. fsync(999) → EBADF (negative).
 * 3. create /tmp/t.txt; write 32 bytes; close.
 *    reopen O_TRUNC; close.
 *    reopen O_RDONLY; read → must return 0 (file truncated). */
```

## 验证

### smoke-linux（M24 全功能）
```bash
make && make esp && make smoke-linux
# 串口含 (无 FAIL)：
#   [user] HelixBusyBoxOK    (M5)
#   [user] HelixLinuxOK      (M5)
#   [user] HelixKbOK         (M18)
#   [user] HelixMouseOK      (M23)
#   [user] HelixCwdOK        (M12)
#   [user] HelixPollOK       ← M24 NEW
#   [user] HelixUnlinkOK     ← M24 NEW
#   [user] HelixFsyncOK      ← M24 NEW
#   [user] HelixSigOK        (M13)
#   [user] [helixbox] HelixPreemptOK   (M22)
```

### smoke-fs — 不回归
```bash
make smoke-fs
# 含 "M4 fs ready" / "HelixFS OK" / "HelixFATWriteOK" / "loaded init+task2 from disk"
```

### silent ENOSYS check
```bash
grep -E "\[syscall\] ENOSYS" serial.log    # 应为空（M24 改 default 为静默）
```

## 已知边界

- **同目录 rename only** — `fat_rename_path` / `ramfs_rename_op` 拒绝跨目录（需要
  拷 cluster chain + 改 `..`)。Linux `rename(2)` POSIX 要求可跨目录；M24 暂实
  现 same-dir，留作 M25+.
- **FAT32 cross-dir cluster 拷贝** — `fat_free_chain` 在 `dir_unlink_at` 已
  用 (walk FAT 写 0)。扩容到 move 时直接复用。
- **LFN (long filename) 删除** — M24 只处理 8.3 dirent。LFN entry 链 (attr 0x0F)
  通过 `attr == 0x0F continue` 自然跳过；但 LFN 没级联 mark 0xE5，留 orphan
  entries。FAT driver 早期阶段容忍 (LFN 的 sum-check 校验在 `find_in_dir` 外)。
- **fsync no-op** — 当前 FAT+ramfs 都 synchronous write；fsync 是 no-op 但通过
  验证 fd 后返回 0。保留 op 字段为未来 buffered fs 留口。
- **silent ENOSYS 副作用** — 任何 default 分支 syscall 都返回 `-ENOSYS` 不打
  log。诊断未实现 syscall 需要在 `serial.log` 间接观察 (BusyBox applet 异常退出
  / `errno = ENOSYS`)。
- **struct vfs_ops 大小敏感** — M24-1 早期 bug: 改完 vfs.h 没 `touch` 所有依赖
  C 文件，ramfs.o 仍 9 字段布局 → 链接后 g_ramfs_ops 末尾 5 字段 garbage → poll
  走错路径。**规则**: 改 vfs.h 后 `touch kernel/fs/*.c kernel/proc/*.c`。
- **headless QEMU** — `-display none` 下 helixbox 的 poll smoke 只验证 poll 路
  径不返回负错误，不验证 wakeup; `pr >= 0` 视为通过。

## 串口 marker 全集

| Marker | 含义 |
|--------|------|
| HelixFATWriteOK | FAT16 写盘 |
| HelixFS OK | init FS 读 hello.txt |
| HelixNetOK | ICMP 网关 |
| HelixTcpOK | TCP 栈 ready |
| HelixBusyBoxOK | BusyBox echo applet |
| HelixLinuxOK | busybox chain done |
| HelixKbOK | 键盘 ring buf |
| HelixMouseOK | PS/2 mouse |
| HelixCwdOK | getcwd/chdir |
| **HelixPollOK** | **poll(2) returns ≥0** |
| **HelixUnlinkOK** | **unlink/rmdir/rename pass** |
| **HelixFsyncOK** | **fsync + fdatasync + O_TRUNC pass** |
| HelixSigOK | SIGCHLD/SIGINT/kill |
| HelixPreemptOK | tick-driven preempt |

加粗 = M24 新增。