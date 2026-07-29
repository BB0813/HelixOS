#pragma once

#include "helix/types.h"

void irq_init(void);                 /* after idt_init + pic_init */
void irq_enable(void);               /* sti */
u64  irq_count(u8 irq);              /* 0..15 */
void irq_dump_stats(void);

/* Internal: IDT dispatch calls this for vectors 32..47. */
void irq_handle(u8 irq);
