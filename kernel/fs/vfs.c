#include "helix/vfs.h"
#include "helix/ramfs.h"
#include "helix/task.h"
#include "helix/serial.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/heap.h"

static const struct vfs_ops *g_root_ops;
static const struct vfs_ops *g_tmp_ops;
static struct vfs_file *g_kernel_fds[VFS_FD_MAX];

static int cons_open(const char *path, int flags, struct vfs_file **out)
{
    (void)path;
    (void)flags;
    (void)out;
    return -1;
}

static int cons_read(struct vfs_file *f, void *buf, u64 len, u64 *out_n)
{
    (void)f;
    (void)buf;
    (void)len;
    *out_n = 0;
    return 0;
}

static int cons_write(struct vfs_file *f, const void *buf, u64 len, u64 *out_n)
{
    (void)f;
    const char *s = buf;
    for (u64 i = 0; i < len; i++)
        serial_putchar(s[i]);
    *out_n = len;
    return 0;
}

static int cons_close(struct vfs_file *f)
{
    (void)f;
    return 0;
}

static const struct vfs_ops g_cons_ops = {
    .open = cons_open,
    .read = cons_read,
    .write = cons_write,
    .close = cons_close,
    .readdir_root = 0,
    .getdents64 = 0,
    .fstat = 0,
    .mkdir = 0,
};

static struct vfs_file g_cons_in  = { .ops = &g_cons_ops, .is_console = 2 };
static struct vfs_file g_cons_out = { .ops = &g_cons_ops, .is_console = 1, .writable = 1 };
static struct vfs_file g_cons_err = { .ops = &g_cons_ops, .is_console = 1, .writable = 1 };

/* Line-buffer user console writes so multi-write lines get one [user] prefix. */
static char g_user_line[256];
static unsigned g_user_line_len;

static void user_line_flush(void)
{
    if (g_user_line_len == 0)
        return;
    serial_puts("[user] ");
    serial_write(g_user_line, g_user_line_len);
    g_user_line_len = 0;
}

static struct vfs_file **fd_table(void)
{
    struct task *t = task_current();
    if (t)
        return t->fds;
    return g_kernel_fds;
}

int vfs_init(void)
{
    g_root_ops = 0;
    g_tmp_ops = 0;
    g_user_line_len = 0;
    memset(g_kernel_fds, 0, sizeof(g_kernel_fds));
    g_kernel_fds[0] = &g_cons_in;
    g_kernel_fds[1] = &g_cons_out;
    g_kernel_fds[2] = &g_cons_err;
    return 0;
}

int vfs_mount_root(const struct vfs_ops *ops)
{
    g_root_ops = ops;
    kprintf("[vfs] root mounted\n");
    return 0;
}

static int is_tmp_path(const char *path)
{
    while (*path == '/')
        path++;
    return path[0] == 't' && path[1] == 'm' && path[2] == 'p' &&
           (path[3] == 0 || path[3] == '/');
}

int vfs_open_flags(const char *path, int flags, struct vfs_file **out)
{
    if (!path || !out)
        return -1;
    if (is_tmp_path(path)) {
        if (!g_tmp_ops || !g_tmp_ops->open)
            return -1;
        return g_tmp_ops->open(path, flags, out);
    }
    if (!g_root_ops || !g_root_ops->open)
        return -1;
    return g_root_ops->open(path, flags, out);
}

int vfs_open(const char *path, struct vfs_file **out)
{
    return vfs_open_flags(path, VFS_O_RDONLY, out);
}

int vfs_read(struct vfs_file *f, void *buf, u64 len, u64 *out_n)
{
    if (!f || !f->ops || !f->ops->read)
        return -1;
    return f->ops->read(f, buf, len, out_n);
}

int vfs_write(struct vfs_file *f, const void *buf, u64 len, u64 *out_n)
{
    if (!f || !f->ops || !f->ops->write)
        return -1;
    return f->ops->write(f, buf, len, out_n);
}

int vfs_close(struct vfs_file *f)
{
    if (!f)
        return -1;
    if (f->is_console)
        return 0;
    if (f->ops && f->ops->close)
        return f->ops->close(f);
    return -1;
}

int vfs_mkdir(const char *path, int mode)
{
    if (!path)
        return -1;
    if (is_tmp_path(path)) {
        if (!g_tmp_ops || !g_tmp_ops->mkdir)
            return -1;
        return g_tmp_ops->mkdir(path, mode);
    }
    if (g_root_ops && g_root_ops->mkdir)
        return g_root_ops->mkdir(path, mode);
    return -1;
}

int vfs_read_all(const char *path, void *buf, u64 cap, u64 *out_n)
{
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0)
        return -1;
    u64 n = 0;
    if (vfs_read(f, buf, cap, &n) != 0) {
        vfs_close(f);
        return -1;
    }
    vfs_close(f);
    *out_n = n;
    return 0;
}

int vfs_root_list(void (*cb)(const char *name, u64 size, void *user), void *user)
{
    if (!g_root_ops || !g_root_ops->readdir_root)
        return -1;
    return g_root_ops->readdir_root(cb, user);
}

void fd_init_task_stdio(void)
{
    struct task *t = task_current();
    if (!t)
        return;
    if (!t->fds[0])
        t->fds[0] = &g_cons_in;
    if (!t->fds[1])
        t->fds[1] = &g_cons_out;
    if (!t->fds[2])
        t->fds[2] = &g_cons_err;
}

int fd_install(struct vfs_file *f)
{
    struct vfs_file **tab = fd_table();
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!tab[i]) {
            tab[i] = f;
            return i;
        }
    }
    return -1;
}

struct vfs_file *fd_get(int fd)
{
    if (fd < 0 || fd >= VFS_FD_MAX)
        return 0;
    return fd_table()[fd];
}

int fd_close(int fd)
{
    if (fd < 0 || fd >= VFS_FD_MAX)
        return -1;
    if (fd <= 2)
        return 0;
    struct vfs_file **tab = fd_table();
    struct vfs_file *f = tab[fd];
    if (!f)
        return -1;
    tab[fd] = 0;
    if (f->is_socket) {
        extern void net_sock_free(void *s);
        net_sock_free(f->fs_priv);
        extern void kfree(void *ptr);
        kfree(f);
        return 0;
    }
    return vfs_close(f);
}

int vfs_console_write(struct vfs_file *f, const char *buf, u64 len)
{
    if (!f || !f->is_console || f->is_console == 2)
        return -1;
    /* Buffer until newline so one logical line → one [user] prefix. */
    for (u64 i = 0; i < len; i++) {
        char c = buf[i];
        if (g_user_line_len + 1 >= sizeof(g_user_line))
            user_line_flush();
        g_user_line[g_user_line_len++] = c;
        if (c == '\n')
            user_line_flush();
    }
    return (int)len;
}

long vfs_getdents64(struct vfs_file *f, void *buf, u64 len)
{
    if (!f || !f->ops || !f->ops->getdents64)
        return -38;
    return f->ops->getdents64(f, buf, len);
}

long vfs_fstat(struct vfs_file *f, void *statbuf)
{
    if (!f || !f->ops || !f->ops->fstat)
        return -38;
    return f->ops->fstat(f, statbuf);
}

/* Called from fs_init after FAT root mount */
void vfs_mount_tmp(const struct vfs_ops *ops)
{
    g_tmp_ops = ops;
    kprintf("[vfs] /tmp mounted (ramfs RW)\n");
}
