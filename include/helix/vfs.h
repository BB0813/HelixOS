#pragma once

#include "helix/types.h"

/* Linux poll(2) event bits (subset). */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct helix_pollfd {
    int fd;
    short events;
    short revents;
};

#define VFS_PATH_MAX  256
#define VFS_FD_MAX    16

/* open flags (Linux subset) */
#define VFS_O_RDONLY  0
#define VFS_O_WRONLY  1
#define VFS_O_RDWR    2
#define VFS_O_CREAT   64
#define VFS_O_TRUNC   512
#define VFS_O_APPEND  1024

struct vfs_file {
    const struct vfs_ops *ops;
    void *fs_priv;
    u64   size;
    u64   pos;
    int   is_console; /* 1=out, 2=in */
    int   is_dir;
    int   writable;
    int   is_socket;  /* 1 = fs_priv is helix_sock* */
    int   refcount;   /* fd slots referencing this file (M11 pipe/shell) */
    int   flags;      /* fcntl flags (O_NONBLOCK etc.) */
};

struct vfs_ops {
    int  (*open)(const char *path, int flags, struct vfs_file **out);
    int  (*read)(struct vfs_file *f, void *buf, u64 len, u64 *out_n);
    int  (*write)(struct vfs_file *f, const void *buf, u64 len, u64 *out_n);
    int  (*close)(struct vfs_file *f);
    int  (*readdir_root)(void (*cb)(const char *name, u64 size, void *user), void *user);
    long (*getdents64)(struct vfs_file *f, void *buf, u64 len);
    long (*fstat)(struct vfs_file *f, void *statbuf);
    int  (*mkdir)(const char *path, int mode);
    /* M24: return POLLIN/POLLOUT mask; 0 if none. Default handler returns
     * POLLIN for read-side and POLLOUT for write-side of regular files. */
    int  (*poll)(struct vfs_file *f);
    /* M24: unlink/rmdir/rename (POSIX subset). Return 0 on success, -1 otherwise. */
    int  (*unlink)(const char *path);
    int  (*rmdir)(const char *path);
    int  (*rename)(const char *oldpath, const char *newpath);
    /* M24: fsync — flush pending writes; 0 on success, -1 on error. */
    int  (*fsync)(struct vfs_file *f);
};

int  vfs_init(void);
int  vfs_mount_root(const struct vfs_ops *ops);
void vfs_mount_tmp(const struct vfs_ops *ops);
int  vfs_open(const char *path, struct vfs_file **out); /* RO compat → flags=0 */
int  vfs_open_flags(const char *path, int flags, struct vfs_file **out);
int  vfs_read(struct vfs_file *f, void *buf, u64 len, u64 *out_n);
int  vfs_write(struct vfs_file *f, const void *buf, u64 len, u64 *out_n);
int  vfs_close(struct vfs_file *f);
int  vfs_mkdir(const char *path, int mode);
/* M24: unlink/rmdir/rename — return 0 on success, -1 on missing/EISDIR/etc.
 * rename() is same-directory only (no cross-dir move). */
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *oldpath, const char *newpath);
int  vfs_read_all(const char *path, void *buf, u64 cap, u64 *out_n);
int  vfs_root_list(void (*cb)(const char *name, u64 size, void *user), void *user);
/* Resolve path against cwd into absolute out[]. Handles ., .., //. Returns 0 or -1. */
int  vfs_path_resolve(const char *cwd, const char *path, char *out, u64 out_cap);
/* 1 if path names an existing directory (openable as dir). */
int  vfs_path_is_dir(const char *path);

int  fd_install(struct vfs_file *f);
struct vfs_file *fd_get(int fd);
int  fd_close(int fd);
void fd_init_task_stdio(void);
/* M11: bump f->refcount (used by dup2/fork when sharing a slot). */
void fd_hold(struct vfs_file *f);
int  vfs_console_write(struct vfs_file *f, const char *buf, u64 len);
long vfs_getdents64(struct vfs_file *f, void *buf, u64 len);
long vfs_fstat(struct vfs_file *f, void *statbuf);
/* M24: poll one file with given events mask; returns revents (POLLIN etc.). */
int  vfs_poll_one(struct vfs_file *f, short events);
