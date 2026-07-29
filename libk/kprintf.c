#include "helix/kprintf.h"
#include "helix/serial.h"
#include "helix/types.h"

static void put_str(const char *s)
{
    serial_puts(s);
}

static void put_unsigned(u64 val, unsigned base, int width, char pad)
{
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;

    if (base < 2 || base > 16)
        base = 10;
    if (val == 0)
        buf[i++] = '0';
    else {
        while (val) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < width)
        buf[i++] = pad;
    while (i--)
        serial_putchar(buf[i]);
}

static void put_signed(i64 val)
{
    if (val < 0) {
        serial_putchar('-');
        /* careful with INT64_MIN */
        put_unsigned((u64)(-(val + 1)) + 1, 10, 0, ' ');
    } else {
        put_unsigned((u64)val, 10, 0, ' ');
    }
}

void kvprintf(const char *fmt, va_list ap)
{
    if (!fmt)
        return;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            serial_putchar(*fmt);
            continue;
        }
        fmt++;
        int long_count = 0;
        while (*fmt == 'l') {
            long_count++;
            fmt++;
        }
        switch (*fmt) {
        case '%':
            serial_putchar('%');
            break;
        case 'c':
            serial_putchar((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            put_str(s ? s : "(null)");
            break;
        }
        case 'd':
        case 'i':
            if (long_count >= 2)
                put_signed(va_arg(ap, i64));
            else if (long_count == 1)
                put_signed(va_arg(ap, long));
            else
                put_signed(va_arg(ap, int));
            break;
        case 'u':
            if (long_count >= 2)
                put_unsigned(va_arg(ap, u64), 10, 0, ' ');
            else if (long_count == 1)
                put_unsigned(va_arg(ap, unsigned long), 10, 0, ' ');
            else
                put_unsigned(va_arg(ap, unsigned), 10, 0, ' ');
            break;
        case 'x':
            if (long_count >= 2)
                put_unsigned(va_arg(ap, u64), 16, 0, ' ');
            else if (long_count == 1)
                put_unsigned(va_arg(ap, unsigned long), 16, 0, ' ');
            else
                put_unsigned(va_arg(ap, unsigned), 16, 0, ' ');
            break;
        case 'p':
            put_str("0x");
            put_unsigned((u64)(uintptr_t)va_arg(ap, void *), 16, 0, ' ');
            break;
        default:
            serial_putchar('%');
            if (*fmt)
                serial_putchar(*fmt);
            break;
        }
        if (!*fmt)
            break;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
