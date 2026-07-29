#include "helix/pic.h"
#include "helix/cpuio.h"
#include "helix/kprintf.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_init(void)
{
    u8 mask1 = inb(PIC1_DATA);
    u8 mask2 = inb(PIC2_DATA);
    (void)mask1;
    (void)mask2;

    /* Start init sequence (cascade, ICW4 needed) */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

    /* ICW2: vector offsets */
    outb(PIC1_DATA, PIC_IRQ_BASE);       /* master 0x20 */
    outb(PIC2_DATA, PIC_IRQ_BASE + 8);   /* slave  0x28 */

    /* ICW3: cascade identity */
    outb(PIC1_DATA, 1u << 2);            /* slave on IRQ2 */
    outb(PIC2_DATA, 2);

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    /* Mask everything; callers unmask what they need */
    pic_mask_all();

    kprintf("[pic] remapped IRQs to 0x%x..0x%x, all masked\n",
            PIC_IRQ_BASE, PIC_IRQ_BASE + 15);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask(u8 irq)
{
    u16 port;
    u8  bit;
    if (irq < 8) {
        port = PIC1_DATA;
        bit = (u8)(1u << irq);
    } else if (irq < 16) {
        port = PIC2_DATA;
        bit = (u8)(1u << (irq - 8));
    } else {
        return;
    }
    outb(port, inb(port) | bit);
}

void pic_unmask(u8 irq)
{
    u16 port;
    u8  bit;
    if (irq < 8) {
        port = PIC1_DATA;
        bit = (u8)(1u << irq);
    } else if (irq < 16) {
        port = PIC2_DATA;
        bit = (u8)(1u << (irq - 8));
        /* also ensure cascade IRQ2 unmasked on master */
        outb(PIC1_DATA, inb(PIC1_DATA) & (u8)~(1u << 2));
    } else {
        return;
    }
    outb(port, inb(port) & (u8)~bit);
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
