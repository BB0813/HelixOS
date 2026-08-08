#include "helix/syscall.h"
#include "helix/errno.h"
#include "helix/task.h"
#include "helix/gdt.h"
#include "helix/vfs.h"
#include "helix/kprintf.h"
#include "helix/serial.h"
#include "helix/string.h"
#include "helix/types.h"
#include "helix/cpuio.h"
#include "helix/heap.h"
#include "helix/pmm.h"
#include "helix/vmm.h"
#include "helix/paging.h"
#include "helix/net.h"
#include "helix/fb.h"
#include "helix/ps2.h"
#include "helix/pipe.h"
#include "helix/signal.h"
#include "helix/timer.h"
#include "helix/tcp.h"

u64 g_syscall_kstack;
u64 g_syscall_user_rsp;

extern void syscall_entry_asm(void);

struct syscall_frame {
    u64 r15, r14, r13, r12, rbp, rbx;  /* callee-saved (preserved across switch) */
    u64 nr;
    u64 a0, a1, a2, a3, a4, a5;
    u64 user_rip;
    u64 user_rflags;
    u64 user_rsp;
};

int user_ptr_ok(const void *ptr, u64 len)
{
    u64 p = (u64)(uintptr_t)ptr;
    if (len && p + len < p)
        return 0;
    /* Helix high window (apps + stacks) */
    if (p >= USER_BASE && p < USER_STACK_TOP && p + len <= USER_STACK_TOP)
        return 1;
    /* ld-helix interpreter window */
    if (p >= 0x50000000ull && p + len <= 0x51000000ull)
        return 1;
    /* Classic low Linux ET_EXEC + brk/mmap (BusyBox data/BSS ~0x4xxxxx–0x9xxxxx) */
    if (p >= USER_LOW_MIN && p + len <= USER_LOW_MAX)
        return 1;
    /* Stacks just below 0x40000000 for low ELFs */
    if (p >= 0x3F000000ull && p + len <= USER_BASE)
        return 1;
    return 0;
}

static int copy_user_path(u64 uptr, char *kbuf, u64 cap)
{
    if (!user_ptr_ok((const void *)(uintptr_t)uptr, 1))
        return -1;
    const char *p = (const char *)(uintptr_t)uptr;
    u64 i = 0;
    while (i + 1 < cap) {
        if (!user_ptr_ok(p + i, 1))
            return -1;
        char c = p[i];
        kbuf[i++] = c;
        if (c == 0)
            return 0;
    }
    return -1;
}

static i64 sys_write(u64 fd, u64 buf, u64 count)
{
    if (count == 0)
        return 0;
    if (!user_ptr_ok((const void *)(uintptr_t)buf, count))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    if (f->is_console)
        return vfs_console_write(f, (const char *)(uintptr_t)buf, count);
    u64 n = 0;
    if (vfs_write(f, (const void *)(uintptr_t)buf, count, &n) != 0)
        return ERR(EACCES);
    return (i64)n;
}

static i64 sys_read(u64 fd, u64 buf, u64 count)
{
    if (count == 0)
        return 0;
    if (!user_ptr_ok((void *)(uintptr_t)buf, count))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    u64 n = 0;
    int r = vfs_read(f, (void *)(uintptr_t)buf, count, &n);
    if (r < 0)
        return (i64)r; /* -EAGAIN etc. from pipes; vfs_read returns negative errno */
    if (r > 0)
        return ERR(EIO);
    return (i64)n;
}

/* Resolve user path against current task cwd into abs[]. */
static int resolve_user_path(u64 uptr, char *abs, u64 abs_cap)
{
    char kpath[VFS_PATH_MAX];
    if (copy_user_path(uptr, kpath, sizeof(kpath)) != 0)
        return -1;
    struct task *t = task_current();
    const char *cwd = (t && t->cwd[0]) ? t->cwd : "/";
    if (vfs_path_resolve(cwd, kpath, abs, abs_cap) != 0)
        return -1;
    return 0;
}

static i64 sys_open(u64 path, u64 flags, u64 mode)
{
    (void)mode;
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    struct vfs_file *f = 0;
    int fl = (int)flags;
    if (vfs_open_flags(abs, fl, &f) != 0)
        return ERR(ENOENT);

    int fd = fd_install(f);
    if (fd < 0) {
        vfs_close(f);
        return ERR(ENOSPC);
    }
    return fd;
}

static i64 sys_mkdir(u64 path, u64 mode)
{
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (vfs_mkdir(abs, (int)mode) != 0)
        return ERR(EACCES);
    return 0;
}

/* M24: unlink/rmdir/rename.
 * Linux NRs: unlink=87, rmdir=84, rename=82. */
static i64 sys_unlink(u64 path)
{
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (vfs_unlink(abs) != 0)
        return ERR(ENOENT);
    return 0;
}

static i64 sys_rmdir(u64 path)
{
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (vfs_rmdir(abs) != 0)
        return ERR(EACCES);
    return 0;
}

static i64 sys_rename(u64 oldp, u64 newp)
{
    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    if (resolve_user_path(oldp, old_abs, sizeof(old_abs)) != 0)
        return ERR(EFAULT);
    if (resolve_user_path(newp, new_abs, sizeof(new_abs)) != 0)
        return ERR(EFAULT);
    if (vfs_rename(old_abs, new_abs) != 0)
        return ERR(EACCES);
    return 0;
}

static i64 sys_openat(u64 dirfd, u64 path, u64 flags, u64 mode)
{
    (void)dirfd; /* only absolute / relative-from-root for M5 */
    return sys_open(path, flags, mode);
}

static i64 sys_close(u64 fd)
{
    return fd_close((int)fd) == 0 ? 0 : ERR(EBADF);
}

/* M24: fsync(74) — flush pending writes for an open fd. HelixOS backends
 * (FAT + ramfs) write through synchronously, so this is effectively a no-op
 * beyond validating the fd. We still call vfs_fsync for any future fs that
 * may buffer. */
static i64 sys_fsync(u64 fd)
{

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    if (f->is_console)
        return 0;
    if (f->ops && f->ops->fsync)
        return f->ops->fsync(f) == 0 ? 0 : ERR(EIO);
    return 0;
}

/* M24: fdatasync(75) — like fsync but skip non-data metadata. Same path
 * under our synchronous backends. */
static i64 sys_fdatasync(u64 fd)
{
    return sys_fsync(fd);
}

static i64 sys_exit(u64 code)
{
    kprintf("[task] pid %d exit(%d)\n",
            task_current() ? task_current()->pid : -1, (int)code);
    task_exit_current((int)code);
    return 0;
}

extern void net_poll(void);

static i64 sys_yield(void)
{
    /* Cooperative yield: poll the NIC so the kernel can process incoming
     * packets while the task is giving up the CPU. Without this, a task
     * that spins on yield would never let the kernel process incoming
     * packets (e.g. TCP SYN+ACK). */
    net_poll();
    task_yield();
    return 0;
}

static i64 sys_getpid(void)
{
    struct task *t = task_current();
    return t ? t->pid : 1;
}

static i64 sys_uname(u64 buf)
{
    if (!user_ptr_ok((void *)(uintptr_t)buf, sizeof(struct helix_utsname)))
        return ERR(EFAULT);
    struct helix_utsname *u = (struct helix_utsname *)(uintptr_t)buf;
    memset(u, 0, sizeof(*u));
    /* Honest policy: sysname is Helix, not Linux (compat disguise would be documented). */
    memcpy(u->sysname, "Helix", 6);
    memcpy(u->nodename, "helix", 6);
    memcpy(u->release, "0.5.0-m5", 9);
    memcpy(u->version, "HelixOS M5 linux-compat subset", 31);
    memcpy(u->machine, "x86_64", 7);
    memcpy(u->domainname, "(none)", 7);
    return 0;
}

static i64 sys_brk(u64 addr)
{
    struct task *t = task_current();
    if (!t)
        return 0;
    if (t->brk_start == 0) {
        t->brk_start = USER_BASE + 0x100000;
        t->brk_curr = t->brk_start;
    }
    if (addr == 0)
        return (i64)t->brk_curr;
    if (addr < t->brk_start)
        return (i64)t->brk_curr;
    /* Ceiling: Helix high apps stop before stacks; classic low ELFs (BusyBox)
     * may brk just above load_end toward 0x3F000000 (below low user stacks). */
    u64 ceiling = (t->brk_start >= USER_BASE)
                      ? (USER_STACK_TOP - USER_STACK_SIZE)
                      : 0x3F000000ull;
    if (addr >= ceiling)
        return (i64)t->brk_curr;
    u64 old = align_up_u64(t->brk_curr, PAGE_SIZE);
    u64 neu = align_up_u64(addr, PAGE_SIZE);
    for (u64 va = old; va < neu; va += PAGE_SIZE) {
        if (!vmm_alloc_user_pages(va, 1, 1))
            return (i64)t->brk_curr;
    }
    t->brk_curr = addr;
    return (i64)t->brk_curr;
}

static i64 sys_getdents64(u64 fd, u64 buf, u64 count)
{
    if (!user_ptr_ok((void *)(uintptr_t)buf, count))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    return vfs_getdents64(f, (void *)(uintptr_t)buf, count);
}

static i64 sys_fstat(u64 fd, u64 statbuf)
{
    if (!user_ptr_ok((void *)(uintptr_t)statbuf, 144))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    return vfs_fstat(f, (void *)(uintptr_t)statbuf);
}

static i64 sys_newfstatat(u64 dirfd, u64 path, u64 statbuf, u64 flags)
{
    (void)dirfd;
    (void)flags;
    if (!user_ptr_ok((void *)(uintptr_t)statbuf, 144))
        return ERR(EFAULT);
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    struct vfs_file *f = 0;
    if (vfs_open(abs, &f) != 0)
        return ERR(ENOENT);
    long r = vfs_fstat(f, (void *)(uintptr_t)statbuf);
    vfs_close(f);
    return r;
}

static i64 sys_ioctl(u64 fd, u64 req, u64 arg)
{
    (void)arg;

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    /* TCGETS etc. — stub success for tty probes on console */
    if (f->is_console)
        return 0;
    (void)req;
    return ERR(ENOSYS);
}

static i64 sys_fcntl(u64 fd, u64 cmd, u64 arg)
{

    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    if (cmd == 1) /* F_GETFD */
        return 0;
    if (cmd == 3) /* F_GETFL */
        return f->flags;
    if (cmd == 4) /* F_SETFL */
        f->flags = (int)arg;
    return 0;
}

static i64 sys_dup2(u64 oldfd, u64 newfd)
{

    struct vfs_file *f = fd_get((int)oldfd);
    if (!f)
        return ERR(EBADF);
    if (newfd >= 16)
        return ERR(EBADF);
    struct task *t = task_current();
    struct vfs_file **tab = t ? t->fds : 0;
    if (!tab)
        return ERR(EBADF);
    if (oldfd == newfd)
        return (i64)newfd;
    if (tab[newfd] && newfd > 2) {
        tab[newfd]->refcount--;
        if (tab[newfd]->refcount <= 0)
            vfs_close(tab[newfd]);
    }
    fd_hold(f);
    tab[newfd] = f;
    return (i64)newfd;
}

/* Linux mmap flags (x86_64) we care about for M6. */
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

static i64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off)
{
    (void)off;
    if (len == 0)
        return ERR(EINVAL);

    /* M18: framebuffer mmap — fd = -4 (0xFFFFFFFFFFFFFFFC) maps GOP fb */
    if (fd == 0xFFFFFFFFFFFFFFFCull) {
        u64 va;
        if (addr == 0) {
            static u64 fb_bump = USER_BASE + 0x1000000ull;
            va = fb_bump;
            fb_bump += align_up_u64(len, PAGE_SIZE);
            if (fb_bump >= USER_STACK_TOP - USER_STACK_SIZE)
                return ERR(ENOMEM);
        } else {
            va = addr & ~0xFFFull;
        }
        if (fb_map_user(va) != 0)
            return ERR(ENOMEM);
        return (i64)va;
    }

    /* File-backed mmap not implemented — anonymous only (MAP_ANONYMOUS or fd==-1). */
    int anon = (flags & MAP_ANONYMOUS) || fd == (u64)-1 || fd == 0xffffffffffffffffull;
    if (!anon)
        return ERR(ENOSYS);
    len = align_up_u64(len, PAGE_SIZE);
    int wr = (prot & 0x2) ? 1 : 1; /* W for simplicity; NX not enforced */
    (void)wr;

    u64 va;
    if (addr == 0 && !(flags & MAP_FIXED)) {
        static u64 anon_bump = USER_BASE + 0x2000000ull; /* high Helix window gap */
        va = anon_bump;
        anon_bump += len;
        if (anon_bump >= USER_STACK_TOP - USER_STACK_SIZE)
            return ERR(ENOMEM);
    } else {
        va = addr & ~0xFFFull;
        /* Fixed / hint: allow Helix high window, classic low ET_EXEC, ld-helix band. */
        int ok_range =
            (va >= USER_BASE && va + len <= USER_STACK_TOP) ||
            (va >= USER_LOW_MIN && va + len <= USER_LOW_MAX) ||
            (va >= 0x50000000ull && va + len <= 0x51000000ull) ||
            (va >= 0x400000ull && va + len <= 0x01000000ull);
        if (!ok_range && (flags & MAP_FIXED))
            return ERR(EINVAL);
        if (!ok_range && addr != 0) {
            /* soft hint outside known windows — still try high bump */
            static u64 anon_bump2 = USER_BASE + 0x2800000ull;
            va = anon_bump2;
            anon_bump2 += len;
            if (anon_bump2 >= USER_STACK_TOP - USER_STACK_SIZE)
                return ERR(ENOMEM);
        }
    }
    if (!vmm_alloc_user_pages(va, len / PAGE_SIZE, 1))
        return ERR(ENOMEM);
    return (i64)va;
}

/* M18: fb_info syscall — returns framebuffer geometry to user. */
struct fb_info_user {
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u64 size;
};
static i64 sys_fb_info(u64 info_ptr)
{
    if (!user_ptr_ok((void *)(uintptr_t)info_ptr, sizeof(struct fb_info_user)))
        return ERR(EFAULT);
    struct fb_info_user *ui = (struct fb_info_user *)(uintptr_t)info_ptr;
    fb_get_info(&ui->width, &ui->height, &ui->pitch, &ui->bpp, &ui->size);
    return 0;
}

/* M18: readkey syscall — non-blocking PS/2 keyboard read. */
static i64 sys_readkey(u64 buf_ptr, u64 len)
{
    if (!len) return 0;
    if (!user_ptr_ok((void *)(uintptr_t)buf_ptr, len))
        return ERR(EFAULT);
    int n = ps2_read((char *)(uintptr_t)buf_ptr, (int)len);
    if (n == 0) return -11; /* EAGAIN */
    return (i64)n;
}

/* M23: read mouse events. buf must hold `count` helix_mouse_event structs.
 * Returns events read, or -EAGAIN if ring buffer empty. */
static i64 sys_mouse_read(u64 buf_ptr, u64 count)
{
    if (!count) return 0;
    if (!user_ptr_ok((void *)(uintptr_t)buf_ptr,
                     count * sizeof(struct helix_mouse_event)))
        return ERR(EFAULT);
    int n = ps2_mouse_read((struct helix_mouse_event *)(uintptr_t)buf_ptr,
                           (int)count);
    if (n == 0) return -11; /* EAGAIN */
    return (i64)n;
}

static i64 sys_getuid(void) { return 0; }
static i64 sys_getgid(void) { return 0; }
static i64 sys_geteuid(void) { return 0; }
static i64 sys_getegid(void) { return 0; }
static i64 sys_setuid(u64 uid) { (void)uid; return 0; }
static i64 sys_setgid(u64 gid) { (void)gid; return 0; }
static i64 sys_getppid(void)
{
    struct task *t = task_current();
    return t && t->parent ? t->parent->pid : 1;
}

/* M11: wait4 — wait for a child; blocks (yields) until it becomes a zombie.
 * pid: 0 = any child, >0 = specific pid, -1 = any child.
 * WNOHANG=1: return immediately if no exited child. */
#define WNOHANG 1
static i64 sys_wait4(u64 pid, u64 status_ptr, u64 options, u64 rusage)
{
    (void)rusage;
    int status = 0;
    int r = task_wait((int)pid, &status, (int)options);
    if (r == -1)
        return ERR(ECHILD);
    if (r == 0)
        return 0; /* WNOHANG, no child ready */
    if (status_ptr && user_ptr_ok((void *)(uintptr_t)status_ptr, 4))
        *(u32 *)(uintptr_t)status_ptr = (u32)status;
    return (i64)r;
}

/* M11: pipe(2) — create a unidirectional pipe, returns [read_fd, write_fd]. */
static i64 sys_pipe(u64 pipefd_ptr)
{
    if (!user_ptr_ok((void *)(uintptr_t)pipefd_ptr, 8))
        return ERR(EFAULT);
    struct helix_pipe *p = helix_pipe_create();
    if (!p)
        return ERR(ENOMEM);
    struct vfs_file *r = helix_pipe_wrap(p, 0); /* read end */
    struct vfs_file *w = helix_pipe_wrap(p, 1); /* write end */
    if (!r || !w) {
        if (r) vfs_close(r);
        if (w) vfs_close(w);
        return ERR(ENOMEM);
    }

    int rf = fd_install(r);
    int wf = fd_install(w);
    if (rf < 0 || wf < 0) {
        if (rf >= 0) fd_close(rf);
        if (wf >= 0) fd_close(wf);
        return ERR(ENOSPC);
    }
    u32 *fds = (u32 *)(uintptr_t)pipefd_ptr;
    fds[0] = (u32)rf;
    fds[1] = (u32)wf;
    return 0;
}
static i64 sys_writev(u64 fd, u64 iov, u64 iovcnt)
{
    /* Minimal: only support single iovec if buffer is user-ok */
    if (iovcnt == 0)
        return 0;
    if (!user_ptr_ok((void *)(uintptr_t)iov, 16))
        return ERR(EFAULT);
    u64 base = *(u64 *)(uintptr_t)iov;
    u64 len = *(u64 *)(uintptr_t)(iov + 8);
    return sys_write(fd, base, len);
}
static i64 sys_stat_path(u64 path, u64 statbuf)
{
    /* Linux x86_64 nr 4 = stat */
    return sys_newfstatat((u64)-100, path, statbuf, 0); /* AT_FDCWD */
}

static i64 sys_munmap(u64 addr, u64 len)
{
    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return ERR(EINVAL);
    u64 start = addr;
    u64 end = addr + len;
    if (end < start)
        return ERR(EINVAL);
    int ok_range =
        (start >= USER_BASE && end <= USER_STACK_TOP) ||
        (start >= USER_LOW_MIN && end <= USER_LOW_MAX) ||
        (start >= 0x50000000ull && end <= 0x51000000ull) ||
        (start >= 0x400000ull && end <= 0x01000000ull);
    if (!ok_range)
        return ERR(EINVAL);
    vmm_unmap_user_range(start, end - start);
    return 0;
}

static i64 sys_mprotect(u64 addr, u64 len, u64 prot)
{
    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return ERR(EINVAL);
    u64 start = addr;
    u64 end = addr + len;
    if (end < start)
        return ERR(EINVAL);
    int ok_range =
        (start >= USER_BASE && end <= USER_STACK_TOP) ||
        (start >= USER_LOW_MIN && end <= USER_LOW_MAX) ||
        (start >= 0x50000000ull && end <= 0x51000000ull) ||
        (start >= 0x400000ull && end <= 0x01000000ull);
    if (!ok_range)
        return ERR(EINVAL);
    if (vmm_set_prot(start, end - start, (int)prot) != 0)
        return ERR(ENOMEM);
    return 0;
}

/* Linux struct sigaction layout we accept (simplified):
 *   u64 sa_handler; u64 sa_flags; u64 sa_restorer; u64 sa_mask; (+pad ok) */
static i64 sys_rt_sigaction(u64 signum, u64 act, u64 oldact, u64 sigsetsize)
{
    (void)sigsetsize;
    int sig = (int)signum;
    if (sig < 1 || sig >= HELIX_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return ERR(EINVAL);
    struct task *t = task_current();
    if (!t)
        return ERR(EINVAL);

    if (oldact) {
        if (!user_ptr_ok((void *)(uintptr_t)oldact, sizeof(struct helix_sigaction)))
            return ERR(EFAULT);
        memcpy((void *)(uintptr_t)oldact, &t->sighand[sig], sizeof(struct helix_sigaction));
    }
    if (act) {
        if (!user_ptr_ok((const void *)(uintptr_t)act, sizeof(struct helix_sigaction)))
            return ERR(EFAULT);
        memcpy(&t->sighand[sig], (const void *)(uintptr_t)act, sizeof(struct helix_sigaction));
    }
    return 0;
}

/* how: SIG_BLOCK=0, SIG_UNBLOCK=1, SIG_SETMASK=2 */
static i64 sys_rt_sigprocmask(u64 how, u64 set, u64 oldset, u64 sigsetsize)
{
    (void)sigsetsize;
    struct task *t = task_current();
    if (!t)
        return ERR(EINVAL);
    if (oldset) {
        if (!user_ptr_ok((void *)(uintptr_t)oldset, 8))
            return ERR(EFAULT);
        *(u64 *)(uintptr_t)oldset = t->sig_blocked;
    }
    if (set) {
        if (!user_ptr_ok((const void *)(uintptr_t)set, 8))
            return ERR(EFAULT);
        u64 news = *(const u64 *)(uintptr_t)set;
        /* Cannot block SIGKILL/SIGSTOP */
        news &= ~((1ull << SIGKILL) | (1ull << SIGSTOP));
        if (how == 0)      /* SIG_BLOCK */
            t->sig_blocked |= news;
        else if (how == 1) /* SIG_UNBLOCK */
            t->sig_blocked &= ~news;
        else if (how == 2) /* SIG_SETMASK */
            t->sig_blocked = news;
        else
            return ERR(EINVAL);
    }
    return 0;
}

static i64 sys_kill(u64 pid, u64 sig)
{
    int s = (int)sig;
    int p = (int)pid;
    if (s < 0 || s >= HELIX_NSIG)
        return ERR(EINVAL);
    /* M13: only positive pid (or 0 = self). No process-group broadcast. */
    if (p < 0)
        return ERR(EINVAL);
    if (p == 0) {
        struct task *cur = task_current();
        if (!cur)
            return ERR(ESRCH);
        p = cur->pid;
    }
    if (s == 0)
        return task_find_by_pid(p) ? 0 : ERR(ESRCH);

    struct task *target = task_find_by_pid(p);
    if (!target || target->state == TASK_ZOMBIE)
        return ERR(ESRCH);
    signal_send(target, s);
    /* Killing self: deliver now so we exit before returning to user. */
    if (target == task_current())
        signal_deliver_current();
    return 0;
}

static i64 sys_set_tid_address(u64 tidptr)
{
    (void)tidptr;
    return task_current() ? task_current()->pid : 1;
}

static i64 sys_arch_prctl(u64 code, u64 addr)
{
    /* ARCH_SET_FS=0x1002, ARCH_GET_FS=0x1003, ARCH_SET_GS=0x1001 */
    if (code == 0x1002) {
        /* SET_FS — write FS_BASE MSR */
        u32 lo = (u32)addr;
        u32 hi = (u32)(addr >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000100), "a"(lo), "d"(hi));
        return 0;
    }
    if (code == 0x1003) {
        if (!user_ptr_ok((void *)(uintptr_t)addr, 8))
            return ERR(EFAULT);
        u32 lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        *(u64 *)(uintptr_t)addr = ((u64)hi << 32) | lo;
        return 0;
    }
    return ERR(EINVAL);
}

static i64 sys_clock_gettime(u64 clkid, u64 tp)
{
    (void)clkid;
    if (!user_ptr_ok((void *)(uintptr_t)tp, 16))
        return ERR(EFAULT);
    extern u64 timer_ticks(void);
    extern u32 timer_hz(void);
    u64 t = timer_ticks();
    u32 hz = timer_hz() ? timer_hz() : 100;
    u64 sec = t / hz;
    u64 nsec = (t % hz) * (1000000000ull / hz);
    u64 *out = (u64 *)(uintptr_t)tp;
    out[0] = sec;
    out[1] = nsec;
    return 0;
}

static i64 sys_getcwd(u64 buf, u64 size)
{
    if (size < 2 || !user_ptr_ok((void *)(uintptr_t)buf, size))
        return ERR(EFAULT);
    struct task *t = task_current();
    const char *cwd = (t && t->cwd[0]) ? t->cwd : "/";
    u64 n = 0;
    while (cwd[n])
        n++;
    if (n + 1 > size)
        return ERR(ERANGE);
    char *b = (char *)(uintptr_t)buf;
    for (u64 i = 0; i < n; i++)
        b[i] = cwd[i];
    b[n] = 0;
    return (i64)buf; /* Linux getcwd returns pointer on success via rax=buf */
}

static i64 sys_chdir(u64 path)
{
    char abs[VFS_PATH_MAX];
    if (resolve_user_path(path, abs, sizeof(abs)) != 0)
        return ERR(EFAULT);
    if (!vfs_path_is_dir(abs))
        return ERR(ENOENT);
    struct task *t = task_current();
    if (!t)
        return ERR(ENOMEM);
    /* Copy abs into t->cwd */
    u64 i = 0;
    while (abs[i] && i + 1 < sizeof(t->cwd)) {
        t->cwd[i] = abs[i];
        i++;
    }
    t->cwd[i] = 0;
    return 0;
}

/* Linux x86_64: socket(AF_INET=2, SOCK_DGRAM=2/SOCK_STREAM=1) */
static i64 sys_socket(u64 domain, u64 type, u64 protocol)
{
    (void)protocol;
    if (domain != 2 /* AF_INET */)
        return ERR(EOPNOTSUPP);


    if (type == 1 /* SOCK_STREAM */) {
        /* M14: TCP socket — allocate via tcp.c */
        extern struct helix_tcp_sock *tcp_alloc_conn(void);
        struct helix_tcp_sock *ts = tcp_alloc_conn();
        if (!ts) return ERR(ENOSPC);
        struct vfs_file *f = kmalloc(sizeof(struct vfs_file));
        if (!f) { extern void tcp_free(struct helix_tcp_sock *); tcp_free(ts); return ERR(ENOMEM); }
        memset(f, 0, sizeof(*f));
        f->is_socket = 2; /* type=T for future identification in bind/send etc. */
        f->fs_priv   = ts;
        int fd = fd_install(f);
        if (fd < 0) { kfree(f); extern void tcp_free(struct helix_tcp_sock *); tcp_free(ts); return ERR(ENOSPC); }
        return fd;
    }
    if (type != 2 /* SOCK_DGRAM */)
        return ERR(EOPNOTSUPP);
    struct helix_sock *s = net_sock_alloc_udp();
    if (!s)
        return ERR(ENOSPC); /* table full */
    /* Allocate a vfs_file wrapper; fs_priv = helix_sock* */
    struct vfs_file *f = kmalloc(sizeof(struct vfs_file));
    if (!f) {
        net_sock_free(s);
        return ERR(ENOMEM);
    }
    memset(f, 0, sizeof(*f));
    f->is_socket = 1;
    f->fs_priv   = s;

    int fd = fd_install(f);
    if (fd < 0) {
        kfree(f);
        net_sock_free(s);
        return ERR(ENOSPC);
    }
    return fd;
}

/* M14: bind supports both UDP and TCP sockets (is_socket=1 UDP, =2 TCP) */
static i64 sys_bind(u64 fd, u64 sockaddr, u64 addrlen)
{
    (void)addrlen;
    if (!user_ptr_ok((const void *)(uintptr_t)sockaddr, 16))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
    const u8 *sa = (const u8 *)(uintptr_t)sockaddr;
    u16 family = (u16)sa[0] | ((u16)sa[1] << 8);
    if (family != 2 /* AF_INET */)
        return ERR(EOPNOTSUPP);
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
    if (f->is_socket == 2) {
        /* TCP socket */
        extern int tcp_bind(struct helix_tcp_sock *, u32, u16);
        int r = tcp_bind((struct helix_tcp_sock *)f->fs_priv, addr_be, port_be);
        return r < 0 ? ERR(-r) : 0;
    }
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    return net_sock_bind(s, addr_be, port_be);
}

static i64 sys_sendto(u64 fd, u64 buf, u64 len, u64 flags, u64 sockaddr, u64 addrlen)
{
    (void)flags; (void)addrlen;
    if (len == 0)
        return 0;
    if (!user_ptr_ok((const void *)(uintptr_t)buf, len))
        return ERR(EFAULT);
    if (!user_ptr_ok((const void *)(uintptr_t)sockaddr, 16))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
    const u8 *sa = (const u8 *)(uintptr_t)sockaddr;
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
    if (f->is_socket == 2) {
        /* TCP: send data via tcp_send_data */
        struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
        (void)port_be; (void)addr_be;
        return tcp_send_data(ts, (const void *)(uintptr_t)buf, (u32)len);
    }
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    return net_sock_sendto(s, (const void *)(uintptr_t)buf,
                           (u32)len, addr_be, port_be);
}

static i64 sys_recvfrom(u64 fd, u64 buf, u64 len, u64 flags, u64 sockaddr, u64 addrlen)
{
    (void)flags; (void)addrlen;
    if (len == 0)
        return 0;
    if (!user_ptr_ok((void *)(uintptr_t)buf, len))
        return ERR(EFAULT);
    if (sockaddr != 0 && !user_ptr_ok((void *)(uintptr_t)sockaddr, 16))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
    if (f->is_socket == 2) {
        /* TCP: recv data via tcp_recv_data */
        struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
        int r = tcp_recv_data(ts, (void *)(uintptr_t)buf, (u32)len);
        if (r < 0)
            return (i64)r;
        if (sockaddr != 0 && r > 0) {
            u8 *sa = (u8 *)(uintptr_t)sockaddr;
            memset(sa, 0, 16);
            sa[0] = 2; sa[1] = 0; /* AF_INET */
            /* remote port (BE) */
            sa[2] = (u8)(ts->rport_be >> 8);
            sa[3] = (u8)ts->rport_be;
            /* remote addr (BE) stored directly */
            sa[4] = (u8)(ts->raddr_be);
            sa[5] = (u8)(ts->raddr_be >> 8);
            sa[6] = (u8)(ts->raddr_be >> 16);
            sa[7] = (u8)(ts->raddr_be >> 24);
        }
        return (i64)r;
    }
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    u32 src_be = 0; u16 sport_be = 0;
    int r = net_sock_recvfrom(s, (void *)(uintptr_t)buf,
                              (u32)len, &src_be, &sport_be);
    if (r < 0)
        return (i64)r; /* -EAGAIN etc */
    if (sockaddr != 0 && r > 0) {
        u8 *sa = (u8 *)(uintptr_t)sockaddr;
        memset(sa, 0, 16);
        sa[0] = 2; sa[1] = 0; /* AF_INET */
        sa[2] = (u8)(sport_be >> 8); sa[3] = (u8)sport_be; /* sport BE */
        sa[4] = (u8)(src_be);  sa[5] = (u8)(src_be >> 8);
        sa[6] = (u8)(src_be >> 16); sa[7] = (u8)(src_be >> 24);
    }
    return (i64)r;
}

/* D5: simple LFSR-based mixer for entropy when RDRAND is unavailable.
 * Mixes TSC, heap address, stack address, and previous output through
 * a 64-bit Galois LFSR. NOT cryptographically secure — but much better
 * than the old (timer_ticks + i*37) deterministic pattern. */
static u64 g_lfsr_state = 0xA5B9D1E3C7F20486ull;

static u8 lfsr_next_byte(void)
{
    /* Galois LFSR feedback: if bit 0 is set, XOR with polynomial */
    if (g_lfsr_state & 1)
        g_lfsr_state = (g_lfsr_state >> 1) ^ 0xB000000000000000ull;
    else
        g_lfsr_state >>= 1;
    /* Stir in external entropy: TSC + address-of-local + heap hint */
    extern u64 timer_ticks(void);
    g_lfsr_state ^= timer_ticks() ^ (u64)&g_lfsr_state;
    return (u8)(g_lfsr_state & 0xFF);
}

static int has_rdrand(void)
{
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx >> 30) & 1;
}

static i64 sys_getrandom(u64 buf, u64 len, u64 flags)
{
    (void)flags;
    if (!user_ptr_ok((void *)(uintptr_t)buf, len))
        return ERR(EFAULT);
    u8 *out = (u8 *)(uintptr_t)buf;
    if (has_rdrand()) {
        for (u64 i = 0; i < len; i++) {
            u64 val;
            /* RDRAND: 64-bit random value; retry up to 10 times on CF=0 */
            int ok = 0;
            for (int retry = 0; retry < 10 && !ok; retry++)
                __asm__ volatile("rdrand %0; setc %%al"
                                 : "=r"(val), "=a"(ok));
            if (!ok) {
                /* RDRAND failed mid-stream — fall back to LFSR for remainder */
                for (; i < len; i++)
                    out[i] = lfsr_next_byte();
                return (i64)len;
            }
            out[i] = (u8)(val & 0xFF);
            /* Stir high bits back into LFSR for fallback path */
            g_lfsr_state ^= val;
        }
    } else {
        /* No RDRAND — use LFSR mixer (better than old deterministic pattern) */
        for (u64 i = 0; i < len; i++)
            out[i] = lfsr_next_byte();
    }
    return (i64)len;
}

/* M24: poll(2) — Linux x86_64 NR=7.
 * User passes an array of `struct helix_pollfd` (8 bytes each: fd, events, revents).
 * Returns count of fds with nonzero revents (POLLIN/POLLOUT/POLLERR/...) on
 * success, 0 on timeout, -EFAULT/-EINVAL on bad args. */
static i64 sys_poll(u64 fds_ptr, u64 nfds, u64 timeout_ms)
{
    if (nfds == 0)
        return 0;
    if (!user_ptr_ok((const void *)(uintptr_t)fds_ptr, nfds * sizeof(struct helix_pollfd)))
        return ERR(EFAULT);
    struct helix_pollfd *pf = (struct helix_pollfd *)(uintptr_t)fds_ptr;
    /* Timeout is ignored for now (cooperative scheduler — caller yields). */
    (void)timeout_ms;
    int ready = 0;
    for (u64 i = 0; i < nfds; i++) {
        if (!user_ptr_ok(&pf[i].revents, sizeof(short)))
            return ERR(EFAULT);
        short ev = pf[i].events;
        short rev = 0;
        if (pf[i].fd < 0) {
            rev = 0; /* POSIX: ignore negative fds */
        } else {
        
            struct vfs_file *f = fd_get(pf[i].fd);
            if (!f) {
                rev = (short)POLLNVAL;
            } else {
                rev = (short)vfs_poll_one(f, ev);
            }
        }
        pf[i].revents = rev;
        if (rev)
            ready++;
    }
    return (i64)ready;
}

/* M24: ppoll(2) — Linux x86_64 NR=271.
 * Like poll(2) but timeout is a `struct timespec*` and a sigmask may be given.
 * We ignore both for the same reason as sys_poll. */
static i64 sys_ppoll(u64 fds_ptr, u64 nfds, u64 tsp_ptr, u64 sigmask_ptr, u64 sigsetsize)
{
    (void)tsp_ptr;
    (void)sigmask_ptr;
    (void)sigsetsize;
    /* Reuse sys_poll, drop the timespec/sigmask (cooperative model). */
    return sys_poll(fds_ptr, nfds, 0);
}

/* TCP stubs — replaced by M14 TCP stack */
#include "helix/tcp.h"

static i64 sys_connect(u64 fd, u64 addr, u64 addrlen)
{
    (void)addrlen;
    if (!user_ptr_ok((const void *)(uintptr_t)addr, 16))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || f->is_socket != 2)
        return ERR(ENOTSOCK);
    struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
    const u8 *sa = (const u8 *)(uintptr_t)addr;
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
    extern int tcp_connect(struct helix_tcp_sock *, u32, u16);
    int r = tcp_connect(ts, addr_be, port_be);
    if (r < 0) return ERR(-r);
    /* connection request sent → return success (async connect) */
    return 0;
}

static i64 sys_accept(u64 fd, u64 addr, u64 addrlen)
{
    (void)addrlen;
    if (addr != 0 && !user_ptr_ok((void *)(uintptr_t)addr, 16))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || f->is_socket != 2)
        return ERR(ENOTSOCK);
    struct helix_tcp_sock *listen_sock = (struct helix_tcp_sock *)f->fs_priv;
    extern struct helix_tcp_sock *tcp_accept(struct helix_tcp_sock *);
    struct helix_tcp_sock *child = tcp_accept(listen_sock);
    if (!child)
        return ERR(EAGAIN); /* no pending connection */
    /* Wrap child in a new FD */
    struct vfs_file *cf = kmalloc(sizeof(struct vfs_file));
    if (!cf) return ERR(ENOMEM);
    memset(cf, 0, sizeof(*cf));
    cf->is_socket = 2;
    cf->fs_priv   = child;
    int cfd = fd_install(cf);
    if (cfd < 0) { kfree(cf); return ERR(ENOSPC); }
    /* Fill remote address */
    if (addr != 0) {
        u8 *sa = (u8 *)(uintptr_t)addr;
        memset(sa, 0, 16);
        sa[0] = 2; sa[1] = 0; /* AF_INET */
        sa[2] = (u8)(child->rport_be >> 8); sa[3] = (u8)child->rport_be;
        sa[4] = (u8)(child->raddr_be);  sa[5] = (u8)(child->raddr_be >> 8);
        sa[6] = (u8)(child->raddr_be >> 16); sa[7] = (u8)(child->raddr_be >> 24);
    }
    return cfd;
}

static i64 sys_listen(u64 fd, u64 backlog)
{
    (void)backlog;

    struct vfs_file *f = fd_get((int)fd);
    if (!f || f->is_socket != 2)
        return ERR(ENOTSOCK);
    struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
    extern int tcp_listen(struct helix_tcp_sock *, u16);
    int r = tcp_listen(ts, ts->lport_be);
    return r < 0 ? ERR(-r) : 0;
}

/*
 * struct msghdr (x86_64 Linux):
 *   +0  void *msg_name        +8  socklen_t msg_namelen
 *  +16  struct iovec *msg_iov +24 size_t    msg_iovlen
 *  +32  void *msg_control    +40 size_t    msg_controllen
 *  +48  int msg_flags
 *
 * struct iovec: +0 void *iov_base  +8 size_t iov_len
 */
static i64 sys_sendmsg(u64 fd, u64 msg, u64 flags)
{
    (void)flags;
    if (!user_ptr_ok((void *)(uintptr_t)msg, 56))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);

    const u8 *m = (const u8 *)(uintptr_t)msg;
    const void *name     = *(const void **)(m);
    u64        namelen   = *(u64 *)(m + 8);
    const void *iovp     = *(const void **)(m + 16);
    u64        iovlen    = *(u64 *)(m + 24);

    if (name && !user_ptr_ok(name, namelen))
        return ERR(EFAULT);

    /* Coalesce all iovecs into a single kernel buffer */
    u8 kbuf[4096];
    u32 total = 0;
    for (u64 i = 0; i < iovlen; i++) {
        if (!user_ptr_ok((const void *)(uintptr_t)iovp, i * 16 + 16))
            return ERR(EFAULT);
        const u8 *iov = (const u8 *)(uintptr_t)iovp + i * 16;
        const void *base = *(const void **)iov;
        u64 len = *(u64 *)(iov + 8);
        if (len == 0) continue;
        if (!user_ptr_ok(base, len))
            return ERR(EFAULT);
        if (total + len > sizeof(kbuf))
            len = sizeof(kbuf) - total;
        memcpy(kbuf + total, base, (u32)len);
        total += (u32)len;
    }
    if (total == 0)
        return 0;

    if (f->is_socket == 2) {
        struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
        return tcp_send_data(ts, kbuf, total);
    }
    /* UDP: parse destination from msg_name */
    const u8 *sa = (const u8 *)name;
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    return net_sock_sendto(s, kbuf, total, addr_be, port_be);
}

static i64 sys_recvmsg(u64 fd, u64 msg, u64 flags)
{
    (void)flags;
    if (!user_ptr_ok((void *)(uintptr_t)msg, 56))
        return ERR(EFAULT);

    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);

    u8 *m = (u8 *)(uintptr_t)msg;
    void  *name      = *(void **)(m);
    u64   namelen    = *(u64 *)(m + 8);
    void  *iovp      = *(void **)(m + 16);
    u64   iovlen     = *(u64 *)(m + 24);

    if (name && !user_ptr_ok(name, namelen))
        return ERR(EFAULT);

    u8 kbuf[4096];
    int r;

    if (f->is_socket == 2) {
        struct helix_tcp_sock *ts = (struct helix_tcp_sock *)f->fs_priv;
        r = tcp_recv_data(ts, kbuf, sizeof(kbuf));
        if (r < 0)
            return (i64)r;
        if (name && namelen >= 16) {
            u8 ua[16];
            memset(ua, 0, 16);
            ua[0] = 2; ua[1] = 0;
            ua[2] = (u8)(ts->rport_be >> 8);
            ua[3] = (u8)ts->rport_be;
            ua[4] = (u8)(ts->raddr_be);
            ua[5] = (u8)(ts->raddr_be >> 8);
            ua[6] = (u8)(ts->raddr_be >> 16);
            ua[7] = (u8)(ts->raddr_be >> 24);
            memcpy(name, ua, 16);
        }
    } else {
        struct helix_sock *s = (struct helix_sock *)f->fs_priv;
        u32 src_be = 0; u16 sport_be = 0;
        r = net_sock_recvfrom(s, kbuf, sizeof(kbuf), &src_be, &sport_be);
        if (r < 0)
            return (i64)r;
        if (name && namelen >= 16 && r > 0) {
            u8 ua[16];
            memset(ua, 0, 16);
            ua[0] = 2; ua[1] = 0;
            ua[2] = (u8)(sport_be >> 8); ua[3] = (u8)sport_be;
            ua[4] = (u8)src_be; ua[5] = (u8)(src_be >> 8);
            ua[6] = (u8)(src_be >> 16); ua[7] = (u8)(src_be >> 24);
            memcpy(name, ua, 16);
        }
    }

    /* Scatter kbuf[0..r-1] into user iovecs */
    if (r > 0) {
        int off = 0;
        for (u64 i = 0; i < iovlen && off < r; i++) {
            if (!user_ptr_ok((void *)(uintptr_t)iovp, i * 16 + 16))
                break;
            u8 *iov = (u8 *)(uintptr_t)iovp + i * 16;
            void *base = *(void **)iov;
            u64 len = *(u64 *)(iov + 8);
            u32 n = (u32)(r - off);
            if (n > len) n = (u32)len;
            if (n > 0 && user_ptr_ok(base, n))
                memcpy(base, kbuf + off, n);
            off += n;
        }
    }
    return (i64)r;
}

static i64 sys_setsockopt_stub(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    /* setsockopt soft-stub: success (TCP_NODELAY etc.) */
    return 0;
}

static i64 sys_getsockopt_stub(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

/* M10: fork — duplicate current process */
static i64 sys_fork(void)
{
    struct task *parent = task_current();
    if (!parent)
        return ERR(ENOMEM);

    struct task *child = task_fork(parent);
    if (!child)
        return ERR(ENOMEM);

    /* Set return values: child gets 0 in rax, parent gets child pid.
     * For the child, we need to modify the saved regs on its kernel stack
     * so that when it returns to user mode, rax=0. */
    /* The child's regs were copied from parent; its rax in the saved context
     * will be restored by syscall_entry path. We set it to 0 here. */
    child->regs.rax = 0;

    /* Parent returns child pid. The child will be scheduled later and its
     * rax=0 will be restored from child->regs.rax when it runs. */
    return (i64)child->pid;
}

/* M10: execve — replace current process image with ELF from path.
 * D4.2: builds a fresh per-task PML4, loads the ELF into it, switches CR3, then
 * destroys the OLD address space. argv is copied from the old user space before
 * the switch so it stays readable regardless of which space becomes active. */
#include "helix/exec.h"
static void exec_free_argv(char *argv[], int argc)
{
    for (int i = 1; i < argc; i++)
        if (argv[i] && argv[i] != argv[0])
            kfree(argv[i]);
}

static i64 sys_execve(u64 pathname, u64 argv_ptr, u64 envp_ptr)
{
    (void)envp_ptr;
    struct task *t = task_current();
    if (!t)
        return ERR(ENOMEM);

    /* Copy pathname from user and resolve against cwd */
    char path[VFS_PATH_MAX];
    if (resolve_user_path(pathname, path, sizeof(path)) != 0)
        return ERR(EFAULT);

    kprintf("[exec] pid=%d execve path=%s\n", t->pid, path);

    /* Read ELF into a kernel buffer (no user-space access). */
    void *buf = 0;
    u64 sz = 0;
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0) {
        kprintf("[exec] open %s failed\n", path);
        return ERR(ENOENT);
    }
    sz = f->size;
    if (sz == 0 || sz > 4 * 1024 * 1024) {
        vfs_close(f);
        return ERR(ENOEXEC);
    }
    buf = kmalloc((size_t)sz);
    if (!buf) {
        vfs_close(f);
        return ERR(ENOMEM);
    }
    u64 n = 0;
    if (vfs_read(f, buf, sz, &n) != 0 || n != sz) {
        kfree(buf);
        vfs_close(f);
        return ERR(ENOEXEC);
    }
    vfs_close(f);

    /* Copy argv strings from the OLD user space while CR3 still points at it.
     * argv[0] is the resolved path (kernel stack buffer). */
    char *argv[16];
    int argc = 1;
    argv[0] = path;
    if (argv_ptr) {
        const u64 *uargv = (const u64 *)(uintptr_t)argv_ptr;
        for (int i = 1; i < 15; i++) {
            u64 p = uargv[i - 1]; /* argv[i] (argv[0] = path already) */
            if (p == 0)
                break;
            if (!user_ptr_ok((const void *)p, 1))
                break;
            char *dst = argv[i] = kmalloc(128);
            if (!dst)
                break;
            int j;
            for (j = 0; j < 127; j++) {
                char c = ((const char *)p)[j];
                dst[j] = c;
                if (c == 0)
                    break;
            }
            dst[j] = 0;
            argc = i + 1;
        }
    }
    argv[argc] = 0;

    /* Build the fresh address space and switch to it. */
    u64 new_pml4 = vmm_clone_kernel_pml4();
    if (!new_pml4) {
        kfree(buf);
        exec_free_argv(argv, argc);
        return ERR(ENOMEM);
    }
    u64 old_pml4 = t->pml4;
    paging_set_pml4(new_pml4);

    struct elf_load_info info;
    int rc = elf_load_dynamic(buf, sz, &info);
    if (rc != 0)
        rc = elf_load_image(buf, sz, &info);
    if (rc != 0) {
        kprintf("[exec] elf_load failed\n");
        paging_set_pml4(old_pml4);
        vmm_destroy_address_space(new_pml4);
        kfree(buf);
        exec_free_argv(argv, argc);
        return ERR(ENOEXEC);
    }
    kfree(buf);

    /* Set up new stack inside new_pml4 (g_pml4 == new_pml4 here). */
    u64 stack_base, stack_top;
    if (info.load_base >= USER_BASE || info.interp_base >= 0x50000000ull) {
        stack_base = USER_STACK_TOP - USER_STACK_SIZE;
        if (!vmm_alloc_user_pages(stack_base, USER_STACK_SIZE / PAGE_SIZE, 1)) {
            paging_set_pml4(old_pml4);
            vmm_destroy_address_space(new_pml4);
            exec_free_argv(argv, argc);
            return ERR(ENOMEM);
        }
        stack_top = stack_base + USER_STACK_SIZE;
    } else {
        stack_top = 0x3FFFF000ull;
        stack_base = stack_top - USER_STACK_SIZE;
        for (u64 va = stack_base; va < stack_top; va += PAGE_SIZE) {
            u64 phys = pmm_alloc_page();
            if (!phys) {
                paging_set_pml4(old_pml4);
                vmm_destroy_address_space(new_pml4);
                exec_free_argv(argv, argc);
                return ERR(ENOMEM);
            }
            memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
            paging_map_4k(va, phys, (1ull) | (1ull << 1) | (1ull << 2));
        }
    }

    u64 sp = setup_user_stack(stack_top, (const char *const *)argv, &info, path);
    exec_free_argv(argv, argc);

    /* Commit: task now runs on new_pml4. CR3 is already there. */
    t->regs.rip = info.entry;
    t->regs.rsp = sp;
    t->regs.rax = 0;
    t->regs.rflags = 0x200;
    t->user_stack_top = stack_top;
    t->brk_start = align_up_u64(info.load_end, PAGE_SIZE);
    t->brk_curr = t->brk_start;
    size_t nlen = strlen(path);
    if (nlen >= TASK_NAME_MAX) nlen = TASK_NAME_MAX - 1;
    memcpy(t->name, path, nlen);
    t->name[nlen] = 0;

    /* Tear down the old address space. CR3 is already on new_pml4, so freeing
     * the old tables (and their user leaves) can't fault the running kernel. */
    t->pml4 = new_pml4;
    t->user_page_count = 0;
    if (old_pml4 && old_pml4 != paging_kernel_pml4())
        vmm_destroy_address_space(old_pml4);

    return 0; /* frame override below returns to new entry on new_pml4 */
}

u64 syscall_entry_c(struct syscall_frame *f)
{
    /* D6: initialize stdio fds once per syscall entry instead of in every handler.
     * Idempotent (checks fd[0..2] before writing). */
    fd_init_task_stdio();

    struct task *t = task_current();
    if (t) {
        t->regs.rip = f->user_rip;
        t->regs.rflags = f->user_rflags;
        t->regs.rsp = f->user_rsp;
        t->regs.rax = f->nr;
        t->regs.rdi = f->a0;
        t->regs.rsi = f->a1;
        t->regs.rdx = f->a2;
        t->regs.r10 = f->a3;
        t->regs.r8  = f->a4;
        t->regs.r9  = f->a5;
        /* Preserve callee-saved user registers across a task switch. */
        t->regs.r15 = f->r15;
        t->regs.r14 = f->r14;
        t->regs.r13 = f->r13;
        t->regs.r12 = f->r12;
        t->regs.rbp = f->rbp;
        t->regs.rbx = f->rbx;
    }

    i64 ret = ERR(ENOSYS);
    switch (f->nr) {
    case SYS_read:        ret = sys_read(f->a0, f->a1, f->a2); break;
    case SYS_write:       ret = sys_write(f->a0, f->a1, f->a2); break;
    case 20: /* writev */ ret = sys_writev(f->a0, f->a1, f->a2); break;
    case 4:  /* stat */   ret = sys_stat_path(f->a0, f->a1); break;
    case 102: /* getuid */ ret = sys_getuid(); break;
    case 104: /* getgid */ ret = sys_getgid(); break;
    case 107: /* geteuid */ ret = sys_geteuid(); break;
    case 108: /* getegid */ ret = sys_getegid(); break;
    case 105: /* setuid */ ret = sys_setuid(f->a0); break;
    case 106: /* setgid */ ret = sys_setgid(f->a0); break;
    case 110: /* getppid */ ret = sys_getppid(); break;
    case SYS_open:        ret = sys_open(f->a0, f->a1, f->a2); break;
    case SYS_close:       ret = sys_close(f->a0); break;
    case SYS_fstat:       ret = sys_fstat(f->a0, f->a1); break;
    case SYS_brk:         ret = sys_brk(f->a0); break;
    case SYS_ioctl:       ret = sys_ioctl(f->a0, f->a1, f->a2); break;
    case SYS_yield:       ret = sys_yield(); break;
    case SYS_dup2:        ret = sys_dup2(f->a0, f->a1); break;
    case SYS_getpid:      ret = sys_getpid(); break;
    case SYS_fcntl:       ret = sys_fcntl(f->a0, f->a1, f->a2); break;
    case SYS_getcwd:      ret = sys_getcwd(f->a0, f->a1); break;
    case SYS_chdir:       ret = sys_chdir(f->a0); break;
    case SYS_mkdir:       ret = sys_mkdir(f->a0, f->a1); break;
    case 82:  /* rename */   ret = sys_rename(f->a0, f->a1); break;
    case 84:  /* rmdir */    ret = sys_rmdir(f->a0); break;
    case 87:  /* unlink */   ret = sys_unlink(f->a0); break;
    case SYS_mmap:        ret = sys_mmap(f->a0, f->a1, f->a2, f->a3, f->a4, f->a5); break;
    case SYS_mprotect:    ret = sys_mprotect(f->a0, f->a1, f->a2); break;
    case SYS_munmap:      ret = sys_munmap(f->a0, f->a1); break;
    case SYS_rt_sigaction: ret = sys_rt_sigaction(f->a0, f->a1, f->a2, f->a3); break;
    case SYS_rt_sigprocmask: ret = sys_rt_sigprocmask(f->a0, f->a1, f->a2, f->a3); break;
    case SYS_kill:        ret = sys_kill(f->a0, f->a1); break;
    case SYS_arch_prctl:   ret = sys_arch_prctl(f->a0, f->a1); break;
    case SYS_set_tid_address: ret = sys_set_tid_address(f->a0); break;
    case SYS_clock_gettime: ret = sys_clock_gettime(f->a0, f->a1); break;
    case SYS_exit:
    case SYS_exit_group:  ret = sys_exit(f->a0); break;
    case SYS_uname:       ret = sys_uname(f->a0); break;
    case SYS_getdents64:  ret = sys_getdents64(f->a0, f->a1, f->a2); break;
    case SYS_openat:      ret = sys_openat(f->a0, f->a1, f->a2, f->a3); break;
    case SYS_newfstatat:  ret = sys_newfstatat(f->a0, f->a1, f->a2, f->a3); break;
    case 41:  /* socket */   ret = sys_socket(f->a0, f->a1, f->a2); break;
    case 44:  /* sendto */   ret = sys_sendto(f->a0, f->a1, f->a2, f->a3, f->a4, f->a5); break;
    case 45:  /* recvfrom */ ret = sys_recvfrom(f->a0, f->a1, f->a2, f->a3, f->a4, f->a5); break;
    case 49:  /* bind */     ret = sys_bind(f->a0, f->a1, f->a2); break;
    case 318: /* getrandom */ret = sys_getrandom(f->a0, f->a1, f->a2); break;
    case 42:  /* connect */   ret = sys_connect(f->a0, f->a1, f->a2); break;
    case 43:  /* accept */    ret = sys_accept(f->a0, f->a1, f->a2); break;
    case 50:  /* listen */    ret = sys_listen(f->a0, f->a1); break;
    case 46:  /* sendmsg */   ret = sys_sendmsg(f->a0, f->a1, f->a2); break;
    case 47:  /* recvmsg */   ret = sys_recvmsg(f->a0, f->a1, f->a2); break;
    case 54:  /* setsockopt */ret = sys_setsockopt_stub(f->a0, f->a1, f->a2, f->a3, f->a4); break;
    case 55:  /* getsockopt */ret = sys_getsockopt_stub(f->a0, f->a1, f->a2, f->a3, f->a4); break;
    case 57:  /* fork */      ret = sys_fork(); break;
    case 59:  /* execve */    ret = sys_execve(f->a0, f->a1, f->a2); break;
    /* M20: explicit ENOSYS for permission/timestamp syscalls (no kprintf spam). */
    case 21:  /* access */     ret = ERR(ENOSYS); break;
    case 90:  /* chmod */      ret = ERR(ENOSYS); break;
    case 91:  /* fchmod */     ret = ERR(ENOSYS); break;
    case 92:  /* chown */      ret = ERR(ENOSYS); break;
    case 93:  /* fchown */     ret = ERR(ENOSYS); break;
    case 94:  /* lchown */     ret = ERR(ENOSYS); break;
    case 132: /* utime */      ret = ERR(ENOSYS); break;
    case 133: /* utimes */     ret = ERR(ENOSYS); break;
    case 280: /* utimensat */  ret = ERR(ENOSYS); break;
    case SYS_wait4:           ret = sys_wait4(f->a0, f->a1, f->a2, f->a3); break;
    case SYS_pipe:            ret = sys_pipe(f->a0); break;
    case SYS_fb_info:         ret = sys_fb_info(f->a0); break;
    case SYS_readkey:         ret = sys_readkey(f->a0, f->a1); break;
    case SYS_mouse_read:      ret = sys_mouse_read(f->a0, f->a1); break;
    case SYS_poll:            ret = sys_poll(f->a0, f->a1, f->a2); break;
    case SYS_ppoll:           ret = sys_ppoll(f->a0, f->a1, f->a2, f->a3, f->a4); break;
    case 74:  /* fsync */     ret = sys_fsync(f->a0); break;
    case 75:  /* fdatasync */ ret = sys_fdatasync(f->a0); break;
    default:
        /* M24: silent ENOSYS — no kprintf spam for unimplemented syscalls.
         * Apps with over-eager probes (e.g. setitimer/timerfd/etc.) used to
         * flood the serial log. They get a clean -ENOSYS now. */
        ret = ERR(ENOSYS);
        break;
    }

    if (t && task_current() == t)
        t->regs.rax = (u64)ret;

    /* Deliver pending signals before returning to userspace. */
    signal_deliver_current();

    /* M22: tick-driven preempt point. If enough ticks have accumulated since
     * last syscall (~80ms) and there is more than one live task, just try
     * to switch. task_yield() returns early if no other task is READY.
     * We deliberately do NOT call net_poll() here — sys_yield() already
     * calls it, and other syscalls that need it call it directly; running
     * it on every syscall return would multiply per-syscall cost by the
     * full TCP/ICMP scan. task_current() below re-reads g_current. */
    if (task_count_alive() > 1 &&
        timer_preempt_pending() >= timer_preempt_threshold()) {
        task_yield();
    }

    t = task_current();
    if (t && t->state == TASK_RUNNING) {
        f->user_rip = t->regs.rip;
        f->user_rflags = t->regs.rflags | 0x200;
        f->user_rsp = t->regs.rsp;
        /* Restore ALL registers of the resumed task (this frame may be a
         * different task's stack, so caller-saved regs too). */
        f->a0 = t->regs.rdi;
        f->a1 = t->regs.rsi;
        f->a2 = t->regs.rdx;
        f->a3 = t->regs.r10;
        f->a4 = t->regs.r8;
        f->a5 = t->regs.r9;
        f->r15 = t->regs.r15;
        f->r14 = t->regs.r14;
        f->r13 = t->regs.r13;
        f->r12 = t->regs.r12;
        f->rbp = t->regs.rbp;
        f->rbx = t->regs.rbx;
        g_syscall_kstack = t->kernel_stack_top;
        ret = (i64)t->regs.rax;
    
    }
    return (u64)ret;
}

void syscall_init(void)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 0);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080), "a"(lo), "d"(hi));

    u32 star_lo = 0;
    u32 star_hi = (0x13u << 16) | 0x08u;
    __asm__ volatile("wrmsr" : : "c"(0xC0000081), "a"(star_lo), "d"(star_hi));

    u64 entry = (u64)(uintptr_t)syscall_entry_asm;
    __asm__ volatile("wrmsr" : : "c"(0xC0000082),
                     "a"((u32)entry), "d"((u32)(entry >> 32)));
    __asm__ volatile("wrmsr" : : "c"(0xC0000084), "a"(0x200u), "d"(0));

    g_syscall_kstack = 0;
    g_syscall_user_rsp = 0;
    kprintf("[syscall] SCE on, LSTAR=0x%llx (M24 poll/ppoll + silent ENOSYS)\n",
            (unsigned long long)entry);
}
