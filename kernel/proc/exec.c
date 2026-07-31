#include "helix/exec.h"
#include "helix/elf.h"
#include "helix/task.h"
#include "helix/vmm.h"
#include "helix/syscall.h"
#include "helix/vfs.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/heap.h"
#include "helix/panic.h"
#include "helix/pmm.h"
#include "helix/paging.h"

static void linux_compat_run_helixbox(void);

static u64 align_down16(u64 x) { return x & ~0xFull; }

static u64 push_str(u64 *sp, const char *s)
{
    u64 n = strlen(s) + 1;
    *sp -= n;
    memcpy((void *)(uintptr_t)(*sp), s, (size_t)n);
    return *sp;
}

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_RANDOM 25
#define AT_EXECFN 31

u64 setup_user_stack(u64 stack_top, const char *const argv[],
                     struct elf_load_info *info, const char *execfn)
{
    u64 sp = align_down16(stack_top);
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    if (argc > 8)
        argc = 8;

    u64 argv_ptr[8];
    for (int i = argc - 1; i >= 0; i--)
        argv_ptr[i] = push_str(&sp, argv[i]);

    u64 execfn_ptr = 0;
    if (execfn)
        execfn_ptr = push_str(&sp, execfn);

    sp = align_down16(sp - 16);
    for (int i = 0; i < 16; i++)
        ((u8 *)(uintptr_t)sp)[i] = (u8)(0x5A ^ i);
    u64 at_random = sp;

    u64 words[64];
    int w = 0;
    words[w++] = (u64)argc;
    for (int i = 0; i < argc; i++)
        words[w++] = argv_ptr[i];
    words[w++] = 0;
    words[w++] = 0;

    words[w++] = AT_PAGESZ; words[w++] = 4096;
    words[w++] = AT_PHENT;  words[w++] = info->phentsize;
    words[w++] = AT_PHNUM;  words[w++] = info->phnum;
    if (info->phdr_addr) {
        words[w++] = AT_PHDR; words[w++] = info->phdr_addr;
    }
    words[w++] = AT_ENTRY;  words[w++] = info->main_entry ? info->main_entry : info->entry;
    if (info->interp_base) {
        words[w++] = AT_BASE; words[w++] = info->interp_base;
    }
    words[w++] = AT_FLAGS;  words[w++] = 0;
    words[w++] = AT_UID;    words[w++] = 0;
    words[w++] = AT_EUID;   words[w++] = 0;
    words[w++] = AT_GID;    words[w++] = 0;
    words[w++] = AT_EGID;   words[w++] = 0;
    words[w++] = AT_RANDOM; words[w++] = at_random;
    if (execfn_ptr) {
        words[w++] = AT_EXECFN; words[w++] = execfn_ptr;
    }
    words[w++] = AT_NULL;   words[w++] = 0;

    sp -= (u64)w * 8;
    sp = align_down16(sp);
    memcpy((void *)(uintptr_t)sp, words, (size_t)w * 8);
    return sp;
}

static int load_path_to_buf(const char *path, void **out_buf, u64 *out_sz)
{
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0)
        return -1;
    u64 sz = f->size;
    if (sz == 0 || sz > 4 * 1024 * 1024) {
        vfs_close(f);
        return -1;
    }
    void *buf = kmalloc((size_t)sz);
    if (!buf) {
        vfs_close(f);
        return -1;
    }
    u64 n = 0;
    if (vfs_read(f, buf, sz, &n) != 0 || n != sz) {
        kfree(buf);
        vfs_close(f);
        return -1;
    }
    vfs_close(f);
    *out_buf = buf;
    *out_sz = sz;
    return 0;
}

static int map_stack(u64 stack_base, u64 stack_top)
{
    for (u64 va = stack_base; va < stack_top; va += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys)
            return -1;
        memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
        if (paging_map_4k(va, phys, (1ull) | (1ull << 1) | (1ull << 2)) != 0)
            return -1;
    }
    return 0;
}

struct task *task_exec_elf(const char *name, const void *elf_img, u64 elf_size,
                           const char *const argv[])
{
    struct elf_load_info info;
    int rc = elf_load_dynamic(elf_img, elf_size, &info);
    if (rc != 0) {
        if (elf_load_image(elf_img, elf_size, &info) != 0)
            return 0;
    }

    u64 stack_base, stack_top;
    if (info.load_base >= USER_BASE || info.interp_base >= 0x50000000ull) {
        static int gen;
        stack_base = USER_STACK_TOP - USER_STACK_SIZE * (u64)((gen++ % 3) + 1);
        if (!vmm_alloc_user_pages(stack_base, USER_STACK_SIZE / PAGE_SIZE, 1))
            return 0;
        stack_top = stack_base + USER_STACK_SIZE;
    } else {
        stack_top = 0x3FFFF000ull;
        stack_base = stack_top - USER_STACK_SIZE;
        if (map_stack(stack_base, stack_top) != 0)
            return 0;
    }

    const char *execfn = (argv && argv[0]) ? argv[0] : name;
    u64 sp = setup_user_stack(stack_top, argv, &info, execfn);
    struct task *t = task_create(name, info.entry, sp);
    if (!t)
        return 0;
    t->brk_start = align_up_u64(info.load_end, PAGE_SIZE);
    t->brk_curr = t->brk_start;
    return t;
}

struct task *task_exec_path(const char *name, const char *path,
                            const char *const argv[])
{
    void *buf = 0;
    u64 sz = 0;
    if (load_path_to_buf(path, &buf, &sz) != 0) {
        kprintf("[exec] read %s failed\n", path);
        return 0;
    }
    kprintf("[exec] read %s ok (%llu bytes)\n", path, (unsigned long long)sz);
    struct task *t = task_exec_elf(name, buf, sz, argv);
    if (!t)
        kprintf("[exec] elf_load/task_create failed for %s\n", path);
    kfree(buf);
    return t;
}

static void linux_compat_run_helixbox(void)
{
    kprintf("[linux] starting helixbox smoke applet\n");
    const char *av[] = { "helixbox", "smoke", 0 };
    task_init();
    struct task *t = task_exec_path("helixbox", "/bin/helixbox", av);
    if (!t) {
        kprintf("[linux] FAIL: cannot exec /bin/helixbox\n");
        extern void kernel_idle_loop(void);
        kernel_idle_loop();
    }
    kprintf("[Helix] M5 linux-compat ready\n");
    task_set_exit_all_hook(msh_compat_run_smoke);
    task_start_user();
}

/* M11/M12: run msh — pipe pipeline + (cwd covered by helixbox HelixCwdOK). */
void msh_compat_run_smoke(void)
{
    kprintf("[Helix] === M11/M12 msh smoke ===\n");
    const char *path = "/bin/msh";
    struct vfs_file *probe = 0;
    if (vfs_open(path, &probe) != 0) {
        kprintf("[msh] %s missing — skip\n", path);
        extern void kernel_idle_loop(void);
        kernel_idle_loop();
        return;
    }
    vfs_close(probe);
    /* msh -c "echo HelixMshOK | cat" — exercises pipe + dup2 + exec + wait */
    const char *av[] = { "msh", "-c", "echo HelixMshOK | cat", 0 };
    task_init();
    struct task *t = task_exec_path("msh", path, av);
    if (!t) {
        kprintf("[msh] FAIL exec /bin/msh\n");
        extern void kernel_idle_loop(void);
        kernel_idle_loop();
        return;
    }
    kprintf("[Helix] M11/M12 msh ready\n");
    task_set_exit_all_hook(0);
    task_start_user();
}

void linux_compat_run_smoke(void)
{
    kprintf("[Helix] === M5 linux-compat smoke ===\n");

    /* Try real static BusyBox multi-call echo first. */
    const char *bb_path = "/bin/busybox";
    struct vfs_file *probe = 0;
    if (vfs_open(bb_path, &probe) == 0) {
        vfs_close(probe);
        kprintf("[linux] trying BusyBox echo\n");
        const char *av[] = { "echo", "HelixBusyBoxOK", 0 };
        task_init();
        struct task *t = task_exec_path("echo", bb_path, av);
        if (t) {
            kprintf("[Helix] M5 busybox ready\n");
            task_set_exit_all_hook(linux_compat_run_helixbox);
            task_start_user();
            return;
        }
        kprintf("[linux] BusyBox exec failed — helixbox only\n");
    }
    linux_compat_run_helixbox();
}

void dyn_compat_run_smoke(void)
{
    kprintf("[Helix] === M6 dyn-link smoke ===\n");
    const char *path = "/bin/hello.dyn";
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0) {
        kprintf("[dyn] %s missing — try musl\n", path);
        musl_compat_run_smoke();
        return;
    }
    vfs_close(f);

    const char *av[] = { "/bin/hello.dyn", 0 };
    task_init();
    struct task *t = task_exec_path("hello.dyn", path, av);
    if (!t) {
        kprintf("[dyn] FAIL exec %s — try musl\n", path);
        musl_compat_run_smoke();
        return;
    }
    kprintf("[Helix] M6 dyn ready — entering user\n");
    task_set_exit_all_hook(musl_compat_run_smoke);
    task_start_user();
}

/* Real musl PIE + ld-musl (NAS-built). After HelloDynOK or if dyn skipped. */
void musl_compat_run_smoke(void)
{
    kprintf("[Helix] === M6 musl dyn smoke ===\n");
    const char *path = "/bin/hello.musl";
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0) {
        kprintf("[musl] %s missing — skip to M5\n", path);
        linux_compat_run_smoke();
        return;
    }
    vfs_close(f);

    /* Prefer real musl loader on disk */
    f = 0;
    if (vfs_open("/lib/ld-musl-x86_64.so.1", &f) != 0) {
        kprintf("[musl] ld-musl missing — skip to M5\n");
        linux_compat_run_smoke();
        return;
    }
    vfs_close(f);

    const char *av[] = { "/bin/hello.musl", 0 };
    task_init();
    struct task *t = task_exec_path("hello.musl", path, av);
    if (!t) {
        kprintf("[musl] FAIL exec %s — skip to M5\n", path);
        linux_compat_run_smoke();
        return;
    }
    kprintf("[Helix] M6 musl ready — entering user\n");
    task_set_exit_all_hook(linux_compat_run_smoke);
    task_start_user();
}

void m5_then_m6_smoke(void)
{
    /* M6 ld-helix → M6 musl → M5 helixbox/BusyBox. */
    dyn_compat_run_smoke();
}
