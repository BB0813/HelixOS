/* M11 pipe(2) implementation — cooperative circular-buffer pipes.
 *
 * Semantics: NON-BLOCKING with EAGAIN (the scheduler only switches tasks at
 * the syscall return path, so a kernel-side yield loop would deadlock).
 *   - read on empty pipe (writer still open)  → -EAGAIN
 *   - read on empty pipe (all writers closed) → 0 (EOF)
 *   - write on full pipe (reader still open)  → short write (what fit) or -EAGAIN
 *   - write with no readers                   → short write
 * Userspace (helixbox applets / msh shell) loops on -EAGAIN with yield().
 */
#include "helix/pipe.h"
#include "helix/heap.h"
#include "helix/string.h"
#include "helix/kprintf.h"
#include "helix/task.h"

/* Linux errno 11 */
#define EAGAIN_PIPE 11

static int pipe_open(const char *path, int flags, struct vfs_file **out)
{
    (void)path; (void)flags; (void)out;
    return -1;
}

static int pipe_read(struct vfs_file *f, void *buf, u64 len, u64 *out_n)
{
    struct helix_pipe *p = (struct helix_pipe *)f->fs_priv;
    if (!p)
        return -1;
    u8 *dst = (u8 *)buf;
    u64 total = 0;
    while (len > 0 && p->count > 0) {
        dst[total++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
        len--;
    }
    if (total > 0) {
        *out_n = total;
        return 0;
    }
    /* Nothing buffered. EOF iff no write slot remains. */
    if (p->write_end && p->write_end->refcount > 0) {
        *out_n = 0;
        return -EAGAIN_PIPE; /* empty, writer still open → retry */
    }
    *out_n = 0;
    return 0; /* EOF */
}

static int pipe_write(struct vfs_file *f, const void *buf, u64 len, u64 *out_n)
{
    struct helix_pipe *p = (struct helix_pipe *)f->fs_priv;
    if (!p)
        return -1;
    /* No reader: short write (like EPIPE without the signal). */
    if (p->read_end && p->read_end->refcount <= 0) {
        *out_n = 0;
        return 0;
    }
    const u8 *src = (const u8 *)buf;
    u64 total = 0;
    while (len > 0 && p->count < PIPE_BUF_SIZE) {
        p->buf[p->head] = src[total++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        len--;
    }
    if (total > 0) {
        *out_n = total;
        return 0;
    }
    /* Full pipe, reader alive → EAGAIN */
    *out_n = 0;
    return -EAGAIN_PIPE;
}

static int pipe_close(struct vfs_file *f)
{
    struct helix_pipe *p = (struct helix_pipe *)f->fs_priv;
    if (!p)
        return -1;
    if (f->writable) {
        if (p->write_end == f)
            p->write_end = 0;
    } else {
        if (p->read_end == f)
            p->read_end = 0;
    }
    int last = !p->read_end && !p->write_end;
    if (last)
        kfree(p);
    kfree(f); /* pipe vfs_file always heap-owned; fd_close does not free it */
    return 0;
}

static long pipe_fstat(struct vfs_file *f, void *statbuf)
{
    (void)f;
    (void)statbuf;
    return -1; /* not a file */
}

static long pipe_getdents(struct vfs_file *f, void *buf, u64 len)
{
    (void)f; (void)buf; (void)len;
    return 0;
}

static const struct vfs_ops pipe_ops = {
    .open = pipe_open,
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .getdents64 = pipe_getdents,
    .fstat = pipe_fstat,
};

struct helix_pipe *helix_pipe_create(void)
{
    struct helix_pipe *p = (struct helix_pipe *)kmalloc(sizeof(*p));
    if (!p)
        return 0;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->read_end = 0;
    p->write_end = 0;
    return p;
}

struct vfs_file *helix_pipe_wrap(struct helix_pipe *p, int is_write)
{
    struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(*f));
    if (!f)
        return 0;
    memset(f, 0, sizeof(*f));
    f->ops = &pipe_ops;
    f->fs_priv = p;
    f->writable = is_write;
    if (is_write)
        p->write_end = f;
    else
        p->read_end = f;
    return f;
}

void helix_pipe_free(struct helix_pipe *p)
{
    if (p)
        kfree(p);
}
