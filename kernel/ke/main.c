#include "helix/boot_info.h"
#include "helix/kprintf.h"
#include "helix/panic.h"
#include "helix/pmm.h"
#include "helix/heap.h"
#include "helix/paging.h"
#include "helix/gdt.h"
#include "helix/idt.h"
#include "helix/irq.h"
#include "helix/timer.h"
#include "helix/shell.h"
#include "helix/syscall.h"
#include "helix/userland.h"
#include "helix/exec.h"
#include "helix/vfs.h"
#include "helix/fs.h"
#include "helix/string.h"
#include "helix/cpuio.h"
#include "helix/net.h"

void kernel_idle_loop(void)
{
    cpu_sti();
    kprintf("[Helix] M3 userland OK\n");
    kprintf("[Helix] kernel idle — shell active\n");
    for (;;) {
        shell_poll();
        timer_poll_heartbeat();
        net_poll();
        cpu_idle();
    }
}

void kernel_early_main(struct helix_boot_info *info)
{
    kprintf("\n[Helix] === early kernel (post ExitBootServices) ===\n");

    if (!info)
        panic("kernel_early_main: null boot_info");

    kprintf("[Helix] mmap entries=%llu phys_ceiling=0x%llx\n",
            (unsigned long long)info->mmap_count,
            (unsigned long long)info->phys_ceiling);

    u64 conventional = 0, boot_svc = 0, other = 0;
    for (u64 i = 0; i < info->mmap_count; i++) {
        u64 bytes = info->mmap[i].npages * 4096ull;
        switch (info->mmap[i].type) {
        case 7: conventional += bytes; break;
        case 3: case 4: boot_svc += bytes; break;
        default: other += bytes; break;
        }
    }
    kprintf("[Helix] mem conventional=%llu MiB boot-svc=%llu MiB other=%llu MiB\n",
            (unsigned long long)(conventional / (1024 * 1024)),
            (unsigned long long)(boot_svc / (1024 * 1024)),
            (unsigned long long)(other / (1024 * 1024)));

    if (pmm_init(info) != 0)
        panic("pmm_init failed");

    /* Keep a low classic hole free of kernel heap so BusyBox-style
     * ET_EXEC (linked at 0x400000, data ~0x711fe0) can use VA=PA. */
    pmm_reserve(0x400000ull, 0xA00000ull);

    if (paging_init_identity(info->phys_ceiling) != 0)
        panic("paging_init_identity failed");

    gdt_init();
    idt_init();

    if (heap_init() != 0)
        panic("heap_init failed");

    u64 a = pmm_alloc_page();
    u64 b = pmm_alloc_page();
    if (!a || !b)
        panic("pmm self-test: alloc failed");
    kprintf("[Helix] pmm self-test pages 0x%llx 0x%llx free=%llu\n",
            (unsigned long long)a, (unsigned long long)b,
            (unsigned long long)pmm_free_pages_count());
    pmm_free_page(a);
    pmm_free_page(b);

    void *p = kmalloc(128);
    void *q = kmalloc(256);
    if (!p || !q)
        panic("kmalloc self-test failed");
    memset(p, 0xA5, 128);
    memset(q, 0x5A, 256);
    kprintf("[Helix] heap self-test p=%p q=%p\n", p, q);
    kfree(p);
    kfree(q);

    kprintf("[Helix] M1 early kernel OK\n");

#ifdef HELIX_M1_TEST_PF
    kprintf("[Helix] deliberate page-fault test\n");
    *(volatile u32 *)(uintptr_t)0x0000400000000000ULL = 0xdeadbeef;
#endif

    irq_init();
    syscall_init();
    vfs_init();
    if (fs_init() != 0)
        kprintf("[Helix] fs_init failed (continuing without disk)\n");
    else
        kprintf("[Helix] M4 fs ready\n");

    if (net_init() != 0)
        kprintf("[Helix] net_init failed (continuing without NIC)\n");

    shell_init(info);
    irq_enable();

    kprintf("[Helix] M2 shell ready (type help)\n");

    /* Give ARP/ICMP self-test a few seconds before long userland smoke. */
    if (net_ready()) {
        u64 start = timer_ticks();
        u32 hz = timer_hz() ? timer_hz() : 100;
        while (timer_ticks() - start < (u64)hz * 8ull) {
            net_poll();
            timer_poll_heartbeat();
            /* brief pause so timer IRQs advance */
            for (volatile int i = 0; i < 10000; i++)
                ;
        }
    }

    /* M3/M4 cooperative demo; on completion hook runs M5 helixbox smoke. */
    userland_start();

    kernel_idle_loop();
}
