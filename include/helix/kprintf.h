#pragma once

#include <stdarg.h>

/* Minimal kernel printf → COM1. Supports %s %c %d %u %x %lx %llx %p %%. */
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
