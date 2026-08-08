#include "helix/ramfs.h"
#include "helix/heap.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/errno.h"

#define RAMFS_MAX_NODES  64
#define RAMFS_MAX_NAME   32
#define RAMFS_MAX_FILE   (64 * 1024)

struct ramfs_node {
    int  used;
    int  is_dir;
    char name[RAMFS_MAX_NAME];
    int  parent;          /* index, -1 = tmp root */
    u8  *data;
    u64  size;
    u64  cap;
};

static struct ramfs_node g_nodes[RAMFS_MAX_NODES];
static int g_tmp_root = -1; /* directory node for /tmp */

struct ramfs_file {
    int  node;
    u64  pos;
};

static int node_alloc(void)
{
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!g_nodes[i].used) {
            memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
            g_nodes[i].used = 1;
            g_nodes[i].parent = -1;
            return i;
        }
    }
    return -1;
}

static int find_child(int parent, const char *name)
{
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!g_nodes[i].used)
            continue;
        if (g_nodes[i].parent == parent && strcmp(g_nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* path relative to /tmp, e.g. "" or "a" or "a/b" */
static int resolve(const char *rel, int need_parent, int *out_parent, char *leaf, int leaf_sz)
{
    int parent = g_tmp_root;
    if (!rel || !*rel) {
        if (need_parent)
            return -1;
        *out_parent = parent;
        if (leaf && leaf_sz)
            leaf[0] = 0;
        return parent;
    }
    char comp[RAMFS_MAX_NAME];
    const char *p = rel;
    for (;;) {
        int n = 0;
        while (*p && *p != '/' && n < RAMFS_MAX_NAME - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/')
            p++;
        if (!*p) {
            /* last component */
            if (need_parent) {
                *out_parent = parent;
                if (leaf) {
                    int i = 0;
                    for (; comp[i] && i < leaf_sz - 1; i++)
                        leaf[i] = comp[i];
                    leaf[i] = 0;
                }
                return 0;
            }
            int c = find_child(parent, comp);
            if (c < 0)
                return -1;
            *out_parent = parent;
            return c;
        }
        int c = find_child(parent, comp);
        if (c < 0 || !g_nodes[c].is_dir)
            return -1;
        parent = c;
    }
}

static int strip_tmp_prefix(const char *path, const char **rel_out)
{
    while (*path == '/')
        path++;
    if (path[0] == 't' && path[1] == 'm' && path[2] == 'p' &&
        (path[3] == 0 || path[3] == '/')) {
        path += 3;
        while (*path == '/')
            path++;
        *rel_out = path;
        return 0;
    }
    return -1;
}

int ramfs_mkdir(const char *path)
{
    const char *rel = 0;
    if (strip_tmp_prefix(path, &rel) != 0)
        return -1;
    if (!*rel) /* /tmp itself */
        return 0;
    int parent = 0;
    char leaf[RAMFS_MAX_NAME];
    if (resolve(rel, 1, &parent, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!leaf[0])
        return -1;
    if (find_child(parent, leaf) >= 0)
        return -1; /* EEXIST */
    int n = node_alloc();
    if (n < 0)
        return -1;
    g_nodes[n].is_dir = 1;
    g_nodes[n].parent = parent;
    memcpy(g_nodes[n].name, leaf, RAMFS_MAX_NAME);
    return 0;
}

int ramfs_create_file(const char *path)
{
    const char *rel = 0;
    if (strip_tmp_prefix(path, &rel) != 0)
        return -1;
    if (!*rel)
        return -1;
    int parent = 0;
    char leaf[RAMFS_MAX_NAME];
    if (resolve(rel, 1, &parent, leaf, sizeof(leaf)) != 0)
        return -1;
    if (find_child(parent, leaf) >= 0)
        return -1;
    int n = node_alloc();
    if (n < 0)
        return -1;
    g_nodes[n].is_dir = 0;
    g_nodes[n].parent = parent;
    memcpy(g_nodes[n].name, leaf, RAMFS_MAX_NAME);
    g_nodes[n].data = 0;
    g_nodes[n].size = 0;
    g_nodes[n].cap = 0;
    return n;
}

static int ramfs_open(const char *path, int flags, struct vfs_file **out)
{
    const char *rel = 0;
    if (strip_tmp_prefix(path, &rel) != 0)
        return -1;

    int idx;
    if (!*rel) {
        idx = g_tmp_root;
    } else {
        int parent = 0;
        idx = resolve(rel, 0, &parent, 0, 0);
        if (idx < 0) {
            if (flags & VFS_O_CREAT) {
                if (ramfs_create_file(path) < 0)
                    return -1;
                idx = resolve(rel, 0, &parent, 0, 0);
            }
            if (idx < 0)
                return -1;
        }
    }

    struct ramfs_node *node = &g_nodes[idx];
    if ((flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_TRUNC | VFS_O_CREAT)) && node->is_dir)
        return -1;
    if (flags & VFS_O_TRUNC && !node->is_dir) {
        node->size = 0;
    }

    struct ramfs_file *rf = kmalloc(sizeof(*rf));
    if (!rf)
        return -1;
    rf->node = idx;
    rf->pos = (flags & VFS_O_APPEND) ? node->size : 0;

    struct vfs_file *vf = kmalloc(sizeof(*vf));
    if (!vf) {
        kfree(rf);
        return -1;
    }
    memset(vf, 0, sizeof(*vf));
    vf->ops = ramfs_vfs_ops();
    vf->fs_priv = rf;
    vf->is_dir = node->is_dir;
    vf->size = node->size;
    vf->pos = rf->pos;
    vf->writable = (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) ? 1 : 0;
    *out = vf;
    return 0;
}

static int ramfs_read(struct vfs_file *f, void *buf, u64 len, u64 *out_n)
{
    struct ramfs_file *rf = f->fs_priv;
    if (!rf || g_nodes[rf->node].is_dir)
        return -1;
    struct ramfs_node *n = &g_nodes[rf->node];
    if (rf->pos >= n->size) {
        *out_n = 0;
        return 0;
    }
    u64 got = n->size - rf->pos;
    if (got > len)
        got = len;
    if (got && n->data)
        memcpy(buf, n->data + rf->pos, (size_t)got);
    rf->pos += got;
    f->pos = rf->pos;
    *out_n = got;
    return 0;
}

static int ramfs_write(struct vfs_file *f, const void *buf, u64 len, u64 *out_n)
{
    struct ramfs_file *rf = f->fs_priv;
    if (!rf || !f->writable || g_nodes[rf->node].is_dir)
        return -1;
    struct ramfs_node *n = &g_nodes[rf->node];
    u64 need = rf->pos + len;
    if (need > RAMFS_MAX_FILE)
        return -1;
    if (need > n->cap) {
        u64 ncap = n->cap ? n->cap * 2 : 256;
        while (ncap < need)
            ncap *= 2;
        if (ncap > RAMFS_MAX_FILE)
            ncap = RAMFS_MAX_FILE;
        u8 *nd = kmalloc((size_t)ncap);
        if (!nd)
            return -1;
        if (n->data && n->size)
            memcpy(nd, n->data, (size_t)n->size);
        if (n->data)
            kfree(n->data);
        n->data = nd;
        n->cap = ncap;
    }
    memcpy(n->data + rf->pos, buf, (size_t)len);
    rf->pos += len;
    if (rf->pos > n->size)
        n->size = rf->pos;
    f->size = n->size;
    f->pos = rf->pos;
    *out_n = len;
    return 0;
}

static int ramfs_close(struct vfs_file *f)
{
    if (f->fs_priv)
        kfree(f->fs_priv);
    kfree(f);
    return 0;
}

static int ramfs_readdir_tmp(void (*cb)(const char *name, u64 size, void *user), void *user)
{
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!g_nodes[i].used || g_nodes[i].parent != g_tmp_root)
            continue;
        cb(g_nodes[i].name, g_nodes[i].is_dir ? 0 : g_nodes[i].size, user);
    }
    return 0;
}

struct linux_dirent64 {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8  d_type;
    char d_name[];
} __attribute__((packed));

static long ramfs_getdents64(struct vfs_file *f, void *buf, u64 len)
{
    if (!f->is_dir)
        return -20;
    struct ramfs_file *rf = f->fs_priv;
    int parent = rf ? rf->node : g_tmp_root;
    u64 produced = 0;
    u8 *out = buf;
    u64 index = f->pos;
    u64 seen = 0;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!g_nodes[i].used || g_nodes[i].parent != parent)
            continue;
        if (seen++ < index)
            continue;
        const char *name = g_nodes[i].name;
        u16 namelen = (u16)strlen(name);
        u16 reclen = (u16)((8 + 8 + 2 + 1 + namelen + 1 + 7) & ~7);
        if (produced + reclen > len) {
            if (produced == 0)
                return -22;
            break;
        }
        struct linux_dirent64 *de = (void *)(out + produced);
        de->d_ino = (u64)(i + 1);
        de->d_off = (i64)(index + 1);
        de->d_reclen = reclen;
        de->d_type = g_nodes[i].is_dir ? 4 : 8;
        memcpy(de->d_name, name, namelen + 1);
        produced += reclen;
        index++;
        f->pos = index;
    }
    return (long)produced;
}

struct helix_stat {
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, __pad;
    u64 st_rdev;
    i64 st_size, st_blksize, st_blocks;
    i64 st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    i64 __unused[3];
};

static long ramfs_fstat(struct vfs_file *f, void *statbuf)
{
    struct ramfs_file *rf = f->fs_priv;
    struct helix_stat *st = statbuf;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_blksize = 512;
    if (f->is_dir) {
        st->st_mode = 0040755;
        st->st_size = 0;
    } else {
        st->st_mode = 0100644;
        st->st_size = (i64)(rf ? g_nodes[rf->node].size : 0);
    }
    return 0;
}

static int ramfs_mkdir_op(const char *path, int mode)
{
    (void)mode;
    return ramfs_mkdir(path) == 0 ? 0 : -1;
}

/* M24: helper to free a node's data buffer and mark slot unused. */
static void node_release(int idx)
{
    if (idx < 0 || idx >= RAMFS_MAX_NODES)
        return;
    if (g_nodes[idx].data) {
        kfree(g_nodes[idx].data);
        g_nodes[idx].data = 0;
    }
    memset(&g_nodes[idx], 0, sizeof(g_nodes[idx]));
}

/* M24: helper — count a node's children (excluding deleted/empty slots). */
static int node_child_count(int idx)
{
    int n = 0;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (g_nodes[i].used && g_nodes[i].parent == idx)
            n++;
    }
    return n;
}

/* M24: unlink a file or empty directory at /tmp/<rel>. Returns 0 or -1. */
static int ramfs_unlink_op(const char *path)
{
    const char *rel = 0;
    if (strip_tmp_prefix(path, &rel) != 0)
        return -1;
    if (!*rel || (rel[0] == '.' && rel[1] == 0))
        return -1; /* cannot unlink /tmp itself or "." */
    int parent = 0;
    char leaf[RAMFS_MAX_NAME];
    if (resolve(rel, 1, &parent, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!leaf[0])
        return -1;
    int idx = find_child(parent, leaf);
    if (idx < 0)
        return -1;
    /* Match POSIX: rmdir-vs-unlink semantics handled at higher layer. */
    node_release(idx);
    return 0;
}

/* M24: rmdir — directory must be empty. */
static int ramfs_rmdir_op(const char *path)
{
    const char *rel = 0;
    if (strip_tmp_prefix(path, &rel) != 0)
        return -1;
    if (!*rel || (rel[0] == '.' && rel[1] == 0))
        return -1;
    int parent = 0;
    char leaf[RAMFS_MAX_NAME];
    if (resolve(rel, 1, &parent, leaf, sizeof(leaf)) != 0)
        return -1;
    if (!leaf[0])
        return -1;
    int idx = find_child(parent, leaf);
    if (idx < 0)
        return -1;
    if (!g_nodes[idx].is_dir)
        return -1; /* not a directory */
    if (node_child_count(idx) > 0)
        return -1; /* not empty */
    node_release(idx);
    return 0;
}

/* M24: rename — same-dir renames the node; cross-dir also reparents it.
 * M24.1: removed the same-parent restriction. */
static int ramfs_rename_op(const char *oldpath, const char *newpath)
{
    const char *old_rel = 0, *new_rel = 0;
    if (strip_tmp_prefix(oldpath, &old_rel) != 0)
        return -1;
    if (strip_tmp_prefix(newpath, &new_rel) != 0)
        return -1;
    if (!*old_rel || !*new_rel)
        return -1;
    int old_parent = 0, new_parent = 0;
    char old_leaf[RAMFS_MAX_NAME], new_leaf[RAMFS_MAX_NAME];
    if (resolve(old_rel, 1, &old_parent, old_leaf, sizeof(old_leaf)) != 0)
        return -1;
    if (resolve(new_rel, 1, &new_parent, new_leaf, sizeof(new_leaf)) != 0)
        return -1;
    if (!old_leaf[0] || !new_leaf[0])
        return -1;
    int idx = find_child(old_parent, old_leaf);
    if (idx < 0)
        return -1;
    /* POSIX: if newpath exists and is a directory, old must also be one
     * (or fail). We keep it minimal — refuse if target exists. */
    if (find_child(new_parent, new_leaf) >= 0)
        return -1;
    /* refuse moving a directory into itself or one of its descendants */
    if (g_nodes[idx].is_dir) {
        for (int p = new_parent; p >= 0; p = g_nodes[p].parent) {
            if (p == idx)
                return -1;
            if (p == g_tmp_root)
                break;
        }
    }
    g_nodes[idx].parent = new_parent;
    memcpy(g_nodes[idx].name, new_leaf, RAMFS_MAX_NAME);
    return 0;
}

static const struct vfs_ops g_ramfs_ops = {
    .open = ramfs_open,
    .read = ramfs_read,
    .write = ramfs_write,
    .close = ramfs_close,
    .readdir_root = ramfs_readdir_tmp,
    .getdents64 = ramfs_getdents64,
    .fstat = ramfs_fstat,
    .mkdir = ramfs_mkdir_op,
    .unlink = ramfs_unlink_op,
    .rmdir = ramfs_rmdir_op,
    .rename = ramfs_rename_op,
};

const struct vfs_ops *ramfs_vfs_ops(void)
{
    return &g_ramfs_ops;
}

int ramfs_init(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    g_tmp_root = node_alloc();
    if (g_tmp_root < 0)
        return -1;
    g_nodes[g_tmp_root].is_dir = 1;
    g_nodes[g_tmp_root].parent = -1;
    memcpy(g_nodes[g_tmp_root].name, "tmp", 4);
    kprintf("[ramfs] /tmp ready (%d nodes max)\n", RAMFS_MAX_NODES);
    return 0;
}
