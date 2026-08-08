#include "helix/timer.h"
#include "helix/pit.h"
#include "helix/pic.h"
#include "helix/kprintf.h"
#include "helix/cpuio.h"

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

/* D7.2: CMOS RTC read — returns Unix epoch seconds (UTC).
 * CMOS ports 0x70 (index) / 0x71 (data). Registers 0..9:
 *   0=sec 1=min 2=hour 3=day-of-week 4=day 5=month 6=year (BCD, 2-digit)
 *   0x0B = status B — bit 2 set means binary mode (we assume BCD).
 * Reads can be in-progress (bit 7 of status A set) — retry until clear. */
static u8 cmos_read(u8 reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static u8 bcd_to_bin(u8 bcd)
{
    return (u8)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

/* Days from 1970-01-01 to YYYY-MM-01 (proleptic Gregorian). */
static u64 days_from_civil_year_month(u64 y, u64 m)
{
    /* Howard Hinnant's algorithm — y/m are 1-indexed month, y=full year */
    y -= (m <= 2);
    u64 era = (y >= 0 ? y : y - 399) / 400;
    u64 yoe = y - era * 400;              /* [0, 399] */
    u64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;  /* [0, 365] */
    u64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0, 146096] */
    return era * 146097 + doe - 719468;  /* days since 1970-01-01 */
}

u64 rtc_unix_seconds(void)
{
    /* Wait for RTC not in update (status A bit 7 clear). */
    while (cmos_read(0x0A) & 0x80) { }
    u8 sec  = cmos_read(0x00);
    u8 min  = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day  = cmos_read(0x07);
    u8 mon  = cmos_read(0x08);
    u8 yr   = cmos_read(0x09);  /* 2-digit year (00..99) */

    u8 status_b = cmos_read(0x0B);
    if (!(status_b & 0x04)) {
        /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
    }
    /* 24-hour mode assumed (status B bit 1); if not, convert 12-hour. */
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = (hour & 0x7F) + 12;
        if (hour >= 24) hour -= 24;
    }

    u64 year = 2000 + (u64)yr;  /* CMOS 2-digit year → 20YY (QEMU epoch 2000+) */
    u64 days = days_from_civil_year_month(year, (u64)mon) + (u64)(day - 1);
    return days * 86400 + (u64)hour * 3600 + (u64)min * 60 + (u64)sec;
}
