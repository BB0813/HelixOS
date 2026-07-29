#include "helix/serial.h"
#include "helix/cpuio.h"
#include "helix/types.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* IER: no IRQ for M2 (poll RX) */
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putchar(char c)
{
    if (c == '\n')
        serial_putchar('\r');
    while ((inb(COM1 + 5) & 0x20) == 0)
        ;
    outb(COM1, (u8)c);
}

void serial_write(const char *s, unsigned long n)
{
    while (n--)
        serial_putchar(*s++);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putchar(*s++);
}

int serial_poll_char(char *out)
{
    if ((inb(COM1 + 5) & 0x01) == 0)
        return 0;
    char c = (char)inb(COM1);
    if (out)
        *out = c;
    return 1;
}
