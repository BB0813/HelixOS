#pragma once

/* COM1 (0x3F8) — valid before and after ExitBootServices. */

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s, unsigned long n);
void serial_puts(const char *s);

/* Non-blocking RX (poll LSR). Returns 1 and stores char, else 0. */
int  serial_poll_char(char *out);

