#pragma once

#include "helix/types.h"

void timer_init(void);
u64  timer_ticks(void);          /* PIT IRQ0 count since init */
u32  timer_hz(void);
/* Called from IRQ0 path only: bump counter, set heartbeat flag. No kprintf. */
void timer_on_irq(void);
/* Main loop: if a second (approx) elapsed, print heartbeat; returns 1 if printed. */
int  timer_poll_heartbeat(void);
/* M22: read-and-clear g_preempt_pending (xchg). Returns accumulated tick
 * count since last call; caller compares against timer_preempt_threshold()
 * to decide whether to preempt on this syscall return. */
int  timer_preempt_pending(void);
/* M22: threshold in ticks for syscall-return preempt (~80ms @100Hz). */
int  timer_preempt_threshold(void);
