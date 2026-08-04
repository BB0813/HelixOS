#include "helix/timer.h"
#include "helix/pit.h"
#include "helix/pic.h"
#include "helix/kprintf.h"

static volatile u64 g_ticks;
static volatile u64 g_heartbeat_due; /* set in IRQ, consumed in poll */
static u32 g_hz = PIT_HZ;

/* M22: 每 tick 累加；syscall 返回路径检查并换 task。
 * 阈值 8 (≈80ms @100Hz) 把 syscall 返回的 preempt 检查开销降到 ~12次/秒，
 * 仍远高于心跳 child (20 dots, 每 dot 一次 yield) 需要的进度粒度。 */
#define PREEMPT_THRESHOLD 8
static volatile int g_preempt_pending;

void timer_init(void)
{
    g_ticks = 0;
    g_heartbeat_due = 0;
    g_preempt_pending = 0;
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
    /* M22: tick flag — syscall return path checks and yields */
    g_preempt_pending++;
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

/* M22: 原子 xchg 读取并清零；返回累计 tick 数。阈值由 caller 决定 */
int timer_preempt_pending(void)
{
    return __atomic_exchange_n(&g_preempt_pending, 0, __ATOMIC_SEQ_CST);
}

/* M22: 当前是否达到 preempt 阈值 (≈80ms @100Hz) */
int timer_preempt_threshold(void)
{
    return PREEMPT_THRESHOLD;
}
