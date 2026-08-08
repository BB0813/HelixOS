#include "helix/userland.h"
#include "helix/elf.h"
#include "helix/task.h"
#include "helix/vmm.h"
#include "helix/paging.h"
#include "helix/syscall.h"
#include "helix/vfs.h"
#include "helix/fs.h"
#include "helix/exec.h"
#include "helix/kprintf.h"
#include "helix/panic.h"
#include "helix/heap.h"
#include "helix/string.h"

#include "generated/user_init_elf.h"
#include "generated/user_task2_elf.h"

static int load_elf_from_path(const char *path, struct elf_load_info *info)
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
    int rc = elf_load_image(buf, sz, info);
    kfree(buf);
    return rc;
}

static void prove_hello_txt(void)
{
    char buf[64];
    memset(buf, 0, sizeof(buf));
    u64 n = 0;
    if (vfs_read_all("/hello.txt", buf, sizeof(buf) - 1, &n) != 0) {
        kprintf("[fs] open/read /hello.txt FAILED\n");
        return;
    }
    buf[n] = 0;
    kprintf("[fs] hello.txt: %s", buf);
    if (n && buf[n - 1] != '\n')
        kprintf("\n");
}

void userland_start(void)
{
    kprintf("[Helix] === M3/M4 userland bring-up ===\n");
    prove_hello_txt();
    task_init();

    /* D4.2: each task owns a per-task PML4. The active CR3 at boot is the
     * kernel template — loading ELFs or stacks into it would pollute
     * g_kernel_pml4 and share user pages across the clones. Load each task's
     * image into its OWN pml4 instead (same pattern as task_exec_elf). */

    /* Preserve the smoke marker when both disk ELFs are present. */
    struct vfs_file *probe = 0;
    int disk_ok = (vfs_open("/bin/init.elf", &probe) == 0);
    if (disk_ok) vfs_close(probe);
    probe = 0;
    disk_ok = disk_ok && (vfs_open("/bin/task2.elf", &probe) == 0);
    if (disk_ok) vfs_close(probe);
    if (disk_ok)
        kprintf("[fs] loaded init+task2 from disk\n");
    else
        kprintf("[fs] disk ELF load failed — embedded fallback\n");

    u64 saved = paging_cr3();
    u64 np = USER_STACK_SIZE / PAGE_SIZE;
    struct elf_load_info info1, info2;

    /* init: fresh per-task pml4, load ELF + stack into it. */
    struct task *init = task_create("init", 0, 0);
    if (!init)
        panic("create init");
    paging_set_pml4(init->pml4);
    if (disk_ok) {
        if (load_elf_from_path("/bin/init.elf", &info1) != 0)
            panic("load init.elf failed");
    } else {
        if (elf_load_image(user_init_elf, user_init_elf_len, &info1) != 0)
            panic("load init.elf failed");
    }
    if (!vmm_alloc_user_pages(USER_STACK_TOP - USER_STACK_SIZE, np, 1))
        panic("user stack1 map failed");
    init->regs.rip = info1.entry;
    init->regs.rsp = USER_STACK_TOP - 16;
    init->user_stack_top = USER_STACK_TOP;
    init->brk_start = align_up_u64(info1.load_end, PAGE_SIZE);
    init->brk_curr = init->brk_start;
    paging_set_pml4(saved);

    /* task2: same, distinct pml4 + stack. */
    struct task *task2 = task_create("task2", 0, 0);
    if (!task2)
        panic("create task2");
    paging_set_pml4(task2->pml4);
    if (disk_ok) {
        if (load_elf_from_path("/bin/task2.elf", &info2) != 0)
            panic("load task2.elf failed");
    } else {
        if (elf_load_image(user_task2_elf, user_task2_elf_len, &info2) != 0)
            panic("load task2.elf failed");
    }
    u64 stack2_base = USER_STACK_TOP - 2 * USER_STACK_SIZE;
    if (!vmm_alloc_user_pages(stack2_base, np, 1))
        panic("user stack2 map failed");
    task2->regs.rip = info2.entry;
    task2->regs.rsp = stack2_base + USER_STACK_SIZE - 16;
    task2->user_stack_top = stack2_base + USER_STACK_SIZE;
    task2->brk_start = align_up_u64(info2.load_end, PAGE_SIZE);
    task2->brk_curr = task2->brk_start;
    paging_set_pml4(saved);

    kprintf("[Helix] starting user tasks (cooperative)\n");
    /* After M3 demo + M5 linux smoke, run M6 dyn smoke then idle. */
    extern void m5_then_m6_smoke(void);
    task_set_exit_all_hook(m5_then_m6_smoke);
    task_start_user();
    panic("task_start_user returned");
}
