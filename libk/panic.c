#include "helix/panic.h"
#include "helix/kprintf.h"
#include "helix/serial.h"

#include <stdarg.h>

void panic(const char *fmt, ...)
{
    serial_puts("\n!!!! HELIX PANIC !!!!\n");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvprintf(fmt, ap);
        va_end(ap);
        serial_puts("\n");
    }
    serial_puts("System halted.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
