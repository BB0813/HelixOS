#pragma once

#include "helix/types.h"

/* 8259A PIC: IRQs remapped to IDT vectors 0x20..0x2F. */
#define PIC_IRQ_BASE    0x20
#define PIC_IRQ_COUNT   16

void pic_init(void);
void pic_eoi(u8 irq);          /* irq 0..15 */
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
void pic_mask_all(void);
