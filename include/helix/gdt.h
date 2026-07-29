#pragma once

#include "helix/types.h"

/* Selectors after gdt_init (RPL baked into user selectors). */
#define GDT_KERNEL_CS  0x08
#define GDT_KERNEL_DS  0x10
#define GDT_USER_DS    0x1B  /* 0x18 | RPL3 */
#define GDT_USER_CS    0x23  /* 0x20 | RPL3 */
#define GDT_TSS        0x28

void gdt_init(void);
void gdt_set_tss_rsp0(u64 rsp0);
u16  gdt_kernel_cs(void);
