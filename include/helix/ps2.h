/* M18 PS/2 keyboard driver. */
#pragma once

#include "helix/types.h"

void ps2_init(void);                     /* unmask IRQ1, flush buffer */
void ps2_handler(void);                  /* called from IRQ1 path */
int  ps2_read(char *buf, int len);       /* non-blocking: returns bytes read, 0 = empty */
