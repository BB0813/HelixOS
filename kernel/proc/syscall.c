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

u64 g_syscall_kstack;
u64 g_syscall_user_rsp;

extern void syscall_entry_asm(void);

struct syscall_frame {
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
    fd_init_task_stdio();
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
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    u64 n = 0;
    if (vfs_read(f, (void *)(uintptr_t)buf, count, &n) != 0)
        return ERR(EIO);
    return (i64)n;
}

static i64 sys_open(u64 path, u64 flags, u64 mode)
{
    (void)mode;
    char kpath[VFS_PATH_MAX];
    if (copy_user_path(path, kpath, sizeof(kpath)) != 0)
        return ERR(EFAULT);
    struct vfs_file *f = 0;
    int fl = (int)flags;
    if (vfs_open_flags(kpath, fl, &f) != 0)
        return ERR(ENOENT);
    fd_init_task_stdio();
    int fd = fd_install(f);
    if (fd < 0) {
        vfs_close(f);
        return ERR(ENOSPC);
    }
    return fd;
}

static i64 sys_mkdir(u64 path, u64 mode)
{
    char kpath[VFS_PATH_MAX];
    if (copy_user_path(path, kpath, sizeof(kpath)) != 0)
        return ERR(EFAULT);
    if (vfs_mkdir(kpath, (int)mode) != 0)
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

static i64 sys_exit(u64 code)
{
    kprintf("[task] pid %d exit(%d)\n",
            task_current() ? task_current()->pid : -1, (int)code);
    task_exit_current((int)code);
    return 0;
}

static i64 sys_yield(void)
{
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
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f)
        return ERR(EBADF);
    return vfs_getdents64(f, (void *)(uintptr_t)buf, count);
}

static i64 sys_fstat(u64 fd, u64 statbuf)
{
    if (!user_ptr_ok((void *)(uintptr_t)statbuf, 144))
        return ERR(EFAULT);
    fd_init_task_stdio();
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
    char kpath[VFS_PATH_MAX];
    if (copy_user_path(path, kpath, sizeof(kpath)) != 0)
        return ERR(EFAULT);
    struct vfs_file *f = 0;
    if (vfs_open(kpath, &f) != 0)
        return ERR(ENOENT);
    long r = vfs_fstat(f, (void *)(uintptr_t)statbuf);
    vfs_close(f);
    return r;
}

static i64 sys_ioctl(u64 fd, u64 req, u64 arg)
{
    (void)arg;
    fd_init_task_stdio();
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
    (void)arg;
    fd_init_task_stdio();
    if (!fd_get((int)fd))
        return ERR(EBADF);
    if (cmd == 1) /* F_GETFD */
        return 0;
    if (cmd == 3) /* F_GETFL */
        return 0;
    return 0; /* soft stub */
}

static i64 sys_dup2(u64 oldfd, u64 newfd)
{
    fd_init_task_stdio();
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
    if (tab[newfd] && newfd > 2)
        vfs_close(tab[newfd]);
    tab[newfd] = f; /* share pointer; M5 no refcount */
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

static i64 sys_getuid(void) { return 0; }
static i64 sys_getgid(void) { return 0; }
static i64 sys_geteuid(void) { return 0; }
static i64 sys_getegid(void) { return 0; }
static i64 sys_setuid(u64 uid) { (void)uid; return 0; }
static i64 sys_setgid(u64 gid) { (void)gid; return 0; }
static i64 sys_getppid(void) { return 1; }
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
    (void)addr;
    (void)len;
    return 0; /* leak pages for now */
}

static i64 sys_mprotect(u64 addr, u64 len, u64 prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

static i64 sys_rt_sigaction(void)
{
    return 0; /* ignore handlers */
}

static i64 sys_rt_sigprocmask(void)
{
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
    char *b = (char *)(uintptr_t)buf;
    b[0] = '/';
    b[1] = 0;
    return (i64)buf;
}

/* Linux x86_64: socket(AF_INET=2, SOCK_DGRAM=2, IPPROTO_UDP=17) */
static i64 sys_socket(u64 domain, u64 type, u64 protocol)
{
    (void)protocol;
    if (domain != 2 /* AF_INET */)
        return ERR(EOPNOTSUPP);
    if (type == 1 /* SOCK_STREAM */) {
        /* TCP stub — not implemented yet */
        return ERR(ENOSYS);
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
    fd_init_task_stdio();
    int fd = fd_install(f);
    if (fd < 0) {
        kfree(f);
        net_sock_free(s);
        return ERR(ENOSPC);
    }
    return fd;
}

static i64 sys_bind(u64 fd, u64 sockaddr, u64 addrlen)
{
    (void)addrlen;
    if (!user_ptr_ok((const void *)(uintptr_t)sockaddr, 16))
        return ERR(EFAULT);
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    /* Parse sockaddr_in */
    const u8 *sa = (const u8 *)(uintptr_t)sockaddr;
    u16 family = (u16)sa[0] | ((u16)sa[1] << 8);
    if (family != 2 /* AF_INET */)
        return ERR(EOPNOTSUPP);
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
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
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
    struct helix_sock *s = (struct helix_sock *)f->fs_priv;
    const u8 *sa = (const u8 *)(uintptr_t)sockaddr;
    u16 port_be = (u16)sa[2] | ((u16)sa[3] << 8);
    u32 addr_be = (u32)sa[4] | ((u32)sa[5] << 8) |
                  ((u32)sa[6] << 16) | ((u32)sa[7] << 24);
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
    fd_init_task_stdio();
    struct vfs_file *f = fd_get((int)fd);
    if (!f || !f->is_socket)
        return ERR(ENOTSOCK);
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

static i64 sys_getrandom(u64 buf, u64 len, u64 flags)
{
    (void)flags;
    if (!user_ptr_ok((void *)(uintptr_t)buf, len))
        return ERR(EFAULT);
    /* Fill with deterministic-ish data (timer ticks) */
    extern u64 timer_ticks(void);
    u8 *out = (u8 *)(uintptr_t)buf;
    u64 t = timer_ticks();
    for (u64 i = 0; i < len; i++)
        out[i] = (u8)(t + i * 37);
    return (i64)len;
}

/* TCP stubs — return ENOSYS until TCP stack is implemented */
static i64 sys_connect_stub(u64 fd, u64 addr, u64 addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    return ERR(ENOSYS);
}

static i64 sys_accept_stub(u64 fd, u64 addr, u64 addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    return ERR(ENOSYS);
}

static i64 sys_listen_stub(u64 fd, u64 backlog)
{
    (void)fd; (void)backlog;
    return ERR(ENOSYS);
}

static i64 sys_sendmsg_stub(u64 fd, u64 msg, u64 flags)
{
    (void)fd; (void)msg; (void)flags;
    return ERR(ENOSYS);
}

static i64 sys_recvmsg_stub(u64 fd, u64 msg, u64 flags)
{
    (void)fd; (void)msg; (void)flags;
    return ERR(ENOSYS);
}

static i64 sys_setsockopt_stub(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return ERR(ENOSYS);
}

static i64 sys_getsockopt_stub(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return ERR(ENOSYS);
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

/* M10: execve — replace current process image with ELF from path */
#include "helix/exec.h"
static i64 sys_execve(u64 pathname, u64 argv_ptr, u64 envp_ptr)
{
    (void)argv_ptr; (void)envp_ptr;
    struct task *t = task_current();
    if (!t)
        return ERR(ENOMEM);

    /* Copy pathname from user */
    char path[128];
    if (!user_ptr_ok((const void *)pathname, 1))
        return ERR(EFAULT);
    const char *src = (const char *)pathname;
    int i;
    for (i = 0; i < 127 && src[i]; i++)
        path[i] = src[i];
    path[i] = 0;

    kprintf("[exec] pid=%d execve path=%s\n", t->pid, path);

    /* Free current user pages */
    task_free_user_pages(t);

    /* Load ELF from path */
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

    /* Load ELF and set up new user space */
    struct elf_load_info info;
    int rc = elf_load_dynamic(buf, sz, &info);
    if (rc != 0) {
        if (elf_load_image(buf, sz, &info) != 0) {
            kfree(buf);
            return ERR(ENOEXEC);
        }
    }
    kfree(buf);

    /* Set up new stack */
    u64 stack_base, stack_top;
    if (info.load_base >= USER_BASE || info.interp_base >= 0x50000000ull) {
        stack_base = USER_STACK_TOP - USER_STACK_SIZE;
        if (!vmm_alloc_user_pages(stack_base, USER_STACK_SIZE / PAGE_SIZE, 1))
            return ERR(ENOMEM);
        stack_top = stack_base + USER_STACK_SIZE;
    } else {
        stack_top = 0x3FFFF000ull;
        stack_base = stack_top - USER_STACK_SIZE;
        /* Map stack pages */
        for (u64 va = stack_base; va < stack_top; va += PAGE_SIZE) {
            u64 phys = pmm_alloc_page();
            if (!phys)
                return ERR(ENOMEM);
            memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
            paging_map_4k(va, phys, (1ull) | (1ull << 1) | (1ull << 2));
            task_track_user_page(t, va, phys);
        }
    }

    /* Build argv from user pointer (simplified: just use path as argv[0]) */
    const char *new_argv[4];
    new_argv[0] = path;
    new_argv[1] = 0;

    /* Set up user stack with auxv */
    u64 sp = stack_top;
    /* Align to16 bytes */
    sp &= ~0xFull;
    /* argc */
    sp -= 8;
    *(u64 *)(uintptr_t)sp = 1; /* argc = 1 */
    /* argv[0] pointer — push path string first */
    u64 path_len = strlen(path) + 1;
    sp -= path_len;
    memcpy((void *)(uintptr_t)sp, path, path_len);
    u64 argv0_ptr = sp;
    /* argv[1] = NULL */
    sp -= 8;
    *(u64 *)(uintptr_t)sp = 0;
    /* envp[0] = NULL */
    sp -= 8;
    *(u64 *)(uintptr_t)sp = 0;
    /* align */
    sp &= ~0xFull;
    /* Push AT_RANDOM (16 bytes) */
    sp -= 16;
    for (int j = 0; j < 16; j++)
        ((u8 *)(uintptr_t)sp)[j] = (u8)(0xAB ^ j);
    u64 at_random = sp;
    /* auxv entries */
    sp -= 8; *(u64 *)(uintptr_t)sp = argv0_ptr; /* back-ptr unused */
    /* AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY, AT_RANDOM, AT_NULL */
    u64 auxv[][2] = {
        { 3,  info.phdr_addr },      /* AT_PHDR */
        { 4,  info.phentsize },      /* AT_PHENT */
        { 5,  info.phnum },          /* AT_PHNUM */
        { 9,  info.main_entry ? info.main_entry : info.entry }, /* AT_ENTRY */
        { 6,  4096 },               /* AT_PAGESZ */
        { 25, at_random },           /* AT_RANDOM */
        { 0,  0 },                   /* AT_NULL */
    };
    for (int j = 0; j < 7; j++) {
        sp -= 16;
        *(u64 *)(uintptr_t)sp = auxv[j][0];
        *(u64 *)(uintptr_t)(sp + 8) = auxv[j][1];
    }
    /* argc + argv[0] pointer */
    sp -= 8;
    *(u64 *)(uintptr_t)sp = 1; /* argc */
    sp -= 8;
    *(u64 *)(uintptr_t)sp = argv0_ptr;

    /* Reset task registers */
    t->regs.rip = info.entry;
    t->regs.rsp = sp;
    t->regs.rax = 0;
    t->regs.rflags = 0x200;
    t->brk_start = align_up_u64(info.load_end, PAGE_SIZE);
    t->brk_curr = t->brk_start;
    /* Copy path into name */
    size_t nlen = strlen(path);
    if (nlen >= TASK_NAME_MAX) nlen = TASK_NAME_MAX - 1;
    memcpy(t->name, path, nlen);
    t->name[nlen] = 0;

    /* We need to force-return to user mode with new regs.
     * Modify the syscall frame so that iret/sysret goes to new entry. */
    return 0; /* will be handled by syscall frame override below */
}

u64 syscall_entry_c(struct syscall_frame *f)
{
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
    case SYS_mkdir:       ret = sys_mkdir(f->a0, f->a1); break;
    case SYS_mmap:        ret = sys_mmap(f->a0, f->a1, f->a2, f->a3, f->a4, f->a5); break;
    case SYS_mprotect:    ret = sys_mprotect(f->a0, f->a1, f->a2); break;
    case SYS_munmap:      ret = sys_munmap(f->a0, f->a1); break;
    case SYS_rt_sigaction: ret = sys_rt_sigaction(); break;
    case SYS_rt_sigprocmask: ret = sys_rt_sigprocmask(); break;
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
    case 42:  /* connect */   ret = sys_connect_stub(f->a0, f->a1, f->a2); break;
    case 43:  /* accept */    ret = sys_accept_stub(f->a0, f->a1, f->a2); break;
    case 50:  /* listen */    ret = sys_listen_stub(f->a0, f->a1); break;
    case 46:  /* sendmsg */   ret = sys_sendmsg_stub(f->a0, f->a1, f->a2); break;
    case 47:  /* recvmsg */   ret = sys_recvmsg_stub(f->a0, f->a1, f->a2); break;
    case 54:  /* setsockopt */ret = sys_setsockopt_stub(f->a0, f->a1, f->a2, f->a3, f->a4); break;
    case 55:  /* getsockopt */ret = sys_getsockopt_stub(f->a0, f->a1, f->a2, f->a3, f->a4); break;
    case 57:  /* fork */      ret = sys_fork(); break;
    case 59:  /* execve */    ret = sys_execve(f->a0, f->a1, f->a2); break;
    default:
        kprintf("[syscall] ENOSYS nr=%llu\n", (unsigned long long)f->nr);
        ret = ERR(ENOSYS);
        break;
    }

    if (t && task_current() == t)
        t->regs.rax = (u64)ret;

    t = task_current();
    if (t && t->state == TASK_RUNNING) {
        f->user_rip = t->regs.rip;
        f->user_rflags = t->regs.rflags | 0x200;
        f->user_rsp = t->regs.rsp;
        g_syscall_kstack = t->kernel_stack_top;
        ret = (i64)t->regs.rax;
        fd_init_task_stdio();
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
    kprintf("[syscall] SCE on, LSTAR=0x%llx (M5 ENOSYS default)\n",
            (unsigned long long)entry);
}
