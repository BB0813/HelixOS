#include "helix/timer.h"
#include "helix/pit.h"
#include "helix/pic.h"
#include "helix/kprintf.h"

static volatile u64 g_ticks;
static volatile u64 g_heartbeat_due; /* set in IRQ, consumed in poll */
static u32 g_hz = PIT_HZ;

void timer_init(void)
{
    g_ticks = 0;
    g_heartbeat_due = 0;
    g_hz = PIT_HZ;
    pit_init(g_hz);
    pic_unmask(0); /* IRQ0 */
    kprintf("[timer] IRQ0 unmasked, hz=%u\n", (unsigned)g_hz);
}

void timer_on_irq(void)
{
    g_ticks++;
    /* Request a heartbeat print every g_hz ticks (~1s). Flag only — no I/O here. */
    if (g_hz && (g_ticks % g_hz) == 0)
        g_heartbeat_due = g_ticks;
}

u64 timer_ticks(void)
{
    return g_ticks;
}

u32 timer_hz(void)
{
    return g_hz;
}

int timer_poll_heartbeat(void)
{
    u64 due = g_heartbeat_due;
    if (!due)
        return 0;
    g_heartbeat_due = 0;
    kprintf("[tick] %llu\n", (unsigned long long)due);
    return 1;
}
