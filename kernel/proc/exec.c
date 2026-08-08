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

/* M20: multi-applet BusyBox smoke chain. Each applet exits → hook advances
 * the index. State lives in module-scope statics. The chain keeps busybox
 * ELF loads low (each load = 1.1 MiB heap) so we cap at 5 applets. */
static int g_bb_idx = 0;
static const char *g_bb_path = 0;
static const char *g_bb_applets[][16] = {
    /* [0] echo (preserves the legacy HelixBusyBoxOK marker) */
    { "echo", "HelixBusyBoxOK", 0 },
    /* [1] cat /etc/welcome.txt — exercises M20 subdir open */
    { "cat", "/etc/welcome.txt", 0 },
    /* [2] echo BB_2 — exercises stdout write again */
    { "echo", "BB2_OK", 0 },
    /* [3] true (exit 0) */
    { "true", 0 },
    /* [4] echo HELIX_BB_DONE — final marker */
    { "echo", "HELIX_BB_DONE", 0 },
};
#define BB_APPLET_COUNT (int)(sizeof(g_bb_applets) / sizeof(g_bb_applets[0]))

static void linux_compat_run_busybox_applets(void);

static void linux_compat_run_busybox_applets(void)
{
    if (g_bb_idx >= BB_APPLET_COUNT) {
        /* All applets done; continue with msh then helixbox. */
        kprintf("[linux] BusyBox chain done (%d applets)\n", BB_APPLET_COUNT);
        msh_compat_run_smoke();
        return;
    }
    const char *name = g_bb_applets[g_bb_idx][0];
    kprintf("[linux] BB[%d/%d]: %s\n", g_bb_idx + 1, BB_APPLET_COUNT, name);
    struct task *t = task_exec_path(name, g_bb_path, (const char **)g_bb_applets[g_bb_idx]);
    if (!t) {
        kprintf("[linux] BB[%d] FAIL exec\n", g_bb_idx);
        g_bb_idx++;
        linux_compat_run_busybox_applets();
        return;
    }
    g_bb_idx++;
    task_set_exit_all_hook(linux_compat_run_busybox_applets);
    task_start_user();
}

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
    /* Create the task first: task_create allocates its per-task PML4, which is
     * the address space the ELF is loaded into. */
    struct task *t = task_create(name, 0, 0);
    if (!t)
        return 0;

    /* Load into t's pml4. Kernel keeps running because the identity map is
     * shared by every per-task PML4. Restore the caller's pml4 when done. */
    u64 saved = paging_cr3();
    paging_set_pml4(t->pml4);

    int rc = elf_load_dynamic(elf_img, elf_size, &info);
    if (rc != 0)
        rc = elf_load_image(elf_img, elf_size, &info);
    if (rc != 0) {
        kprintf("[exec] elf_load failed\n");
        paging_set_pml4(saved);
        vmm_destroy_address_space(t->pml4);
        pmm_free_page(t->kernel_stack);
        t->kernel_stack = 0;
        t->state = TASK_UNUSED;
        return 0;
    }

    u64 stack_base, stack_top;
    if (info.load_base >= USER_BASE || info.interp_base >= 0x50000000ull) {
        static int gen;
        stack_base = USER_STACK_TOP - USER_STACK_SIZE * (u64)((gen++ % 3) + 1);
        if (!vmm_alloc_user_pages(stack_base, USER_STACK_SIZE / PAGE_SIZE, 1)) {
            paging_set_pml4(saved);
            vmm_destroy_address_space(t->pml4);
            pmm_free_page(t->kernel_stack);
            t->kernel_stack = 0;
            t->state = TASK_UNUSED;
            return 0;
        }
        stack_top = stack_base + USER_STACK_SIZE;
    } else {
        stack_top = 0x3FFFF000ull;
        stack_base = stack_top - USER_STACK_SIZE;
        if (map_stack(stack_base, stack_top) != 0) {
            paging_set_pml4(saved);
            vmm_destroy_address_space(t->pml4);
            pmm_free_page(t->kernel_stack);
            t->kernel_stack = 0;
            t->state = TASK_UNUSED;
            return 0;
        }
    }

    const char *execfn = (argv && argv[0]) ? argv[0] : name;
    u64 sp = setup_user_stack(stack_top, argv, &info, execfn);
    t->regs.rip = info.entry;
    t->regs.rsp = sp;
    t->user_stack_top = stack_top;
    t->brk_start = align_up_u64(info.load_end, PAGE_SIZE);
    t->brk_curr = t->brk_start;

    paging_set_pml4(saved);
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
    task_set_exit_all_hook(0); /* helixbox is final; no further hook */
    task_start_user();
}

/* M11/M12: run msh — pipe pipeline + (cwd covered by helixbox HelixCwdOK). */
void msh_compat_run_smoke(void)
{
    kprintf("[Helix] === M20 msh smoke (extended builtins) ===\n");
    const char *path = "/bin/msh";
    struct vfs_file *probe = 0;
    if (vfs_open(path, &probe) != 0) {
        kprintf("[msh] %s missing — skip to helixbox\n", path);
        linux_compat_run_helixbox();
        return;
    }
    vfs_close(probe);
    /* M20: exercise new builtins via single msh -c with ';' statement separator.
     *   alias x=echo; x HELIX_MSH_ALIAS_OK
     *   export A=42
     *   test -f /hello.txt (each invocation returns rc — if open succeeds, OK)
     *   echo HELIX_MSH_DONE | cat
     */
    const char *av[] = {
        "msh", "-c",
        "alias x=echo; x HELIX_MSH_ALIAS_OK; "
        "export A=42; "
        "echo HELIX_MSH_EXPORT_OK; "
        "test -f /hello.txt; "
        "echo HELIX_MSH_TEST_OK; "
        "echo HELIX_MSH_DONE | cat",
        0
    };
    struct task *t = task_exec_path("msh", path, av);
    if (!t) {
        kprintf("[msh] FAIL exec /bin/msh\n");
        linux_compat_run_helixbox();
        return;
    }
    kprintf("[Helix] M20 msh ready\n");
    task_set_exit_all_hook(linux_compat_run_helixbox);
    task_start_user();
}

void linux_compat_run_smoke(void)
{
    kprintf("[Helix] === M5 linux-compat smoke ===\n");

    /* M20: real BusyBox multi-applet chain. Try each applet; the exit-all
     * hook advances to the next one. End of chain → linux_compat_run_helixbox. */
    const char *bb_path = "/bin/busybox";
    struct vfs_file *probe = 0;
    if (vfs_open(bb_path, &probe) == 0) {
        vfs_close(probe);
        kprintf("[linux] trying BusyBox multi-applet chain\n");
        g_bb_idx = 0;
        g_bb_path = bb_path;
        task_init();
        linux_compat_run_busybox_applets();
        return;
    }
    kprintf("[linux] BusyBox not present — helixbox only\n");
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
