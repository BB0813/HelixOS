#pragma once

#include <stddef.h>

int   heap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
