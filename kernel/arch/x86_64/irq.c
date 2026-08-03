#include "helix/irq.h"
#include "helix/pic.h"
#include "helix/timer.h"
#include "helix/cpuio.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/ps2.h"

static volatile u64 g_irq_counts[PIC_IRQ_COUNT];

void irq_handle(u8 irq)
{
    if (irq < PIC_IRQ_COUNT)
        g_irq_counts[irq]++;

    switch (irq) {
    case 0:
        timer_on_irq();
        break;
    case 1:
        ps2_handler();
        break;
    default:
        /* No handler: still EOI so the PIC does not stick. */
        break;
    }

    pic_eoi(irq);
}

void irq_init(void)
{
    memset((void *)g_irq_counts, 0, sizeof(g_irq_counts));
    /* Gates 32..47 already installed in idt_init. */
    pic_init();
    timer_init();
    ps2_init();
    kprintf("[irq] PIC+PIT ready (IRQ0 timer, IRQ1 keyboard)\n");
}

void irq_enable(void)
{
    cpu_sti();
    kprintf("[irq] STI — interrupts enabled\n");
}

u64 irq_count(u8 irq)
{
    if (irq >= PIC_IRQ_COUNT)
        return 0;
    return g_irq_counts[irq];
}

void irq_dump_stats(void)
{
    kprintf("IRQ counts (PIC 0..15):\n");
    for (u8 i = 0; i < PIC_IRQ_COUNT; i++) {
        u64 c = g_irq_counts[i];
        if (c)
            kprintf("  irq%u: %llu\n", (unsigned)i, (unsigned long long)c);
    }
    kprintf("  (zero counts omitted)\n");
}
