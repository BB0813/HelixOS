#include "helix/userland.h"
#include "helix/elf.h"
#include "helix/task.h"
#include "helix/vmm.h"
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

    struct elf_load_info info1, info2;
    if (load_elf_from_path("/bin/init.elf", &info1) == 0 &&
        load_elf_from_path("/bin/task2.elf", &info2) == 0) {
        kprintf("[fs] loaded init+task2 from disk\n");
    } else {
        kprintf("[fs] disk ELF load failed — embedded fallback\n");
        if (elf_load_image(user_init_elf, user_init_elf_len, &info1) != 0)
            panic("load init.elf failed");
        if (elf_load_image(user_task2_elf, user_task2_elf_len, &info2) != 0)
            panic("load task2.elf failed");
    }

    u64 stack1_base = USER_STACK_TOP - USER_STACK_SIZE;
    u64 stack2_base = USER_STACK_TOP - 2 * USER_STACK_SIZE;
    u64 np = USER_STACK_SIZE / PAGE_SIZE;
    if (!vmm_alloc_user_pages(stack1_base, np, 1))
        panic("user stack1 map failed");
    if (!vmm_alloc_user_pages(stack2_base, np, 1))
        panic("user stack2 map failed");
    u64 sp1 = USER_STACK_TOP - 16;
    u64 sp2 = stack2_base + USER_STACK_SIZE - 16;

    task_init();
    if (!task_create("init", info1.entry, sp1))
        panic("create init");
    if (!task_create("task2", info2.entry, sp2))
        panic("create task2");

    kprintf("[Helix] starting user tasks (cooperative)\n");
    /* After M3 demo + M5 linux smoke, run M6 dyn smoke then idle. */
    extern void m5_then_m6_smoke(void);
    task_set_exit_all_hook(m5_then_m6_smoke);
    task_start_user();
    panic("task_start_user returned");
}
