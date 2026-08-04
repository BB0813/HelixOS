/* M18 PS/2 keyboard + M23 PS/2 mouse driver. */
#pragma once

#include "helix/types.h"

void ps2_init(void);                     /* unmask IRQ1 + IRQ12, flush buffer */
void ps2_handler(void);                  /* called from IRQ1 path */
int  ps2_read(char *buf, int len);       /* non-blocking: returns bytes read, 0 = empty */

/* M23: PS/2 mouse events.
 * dx/dy are accumulated delta since last packet.
 * buttons: bit0=左键, bit1=右键, bit2=中键. */
struct helix_mouse_event {
    i16 dx, dy;
    u8  buttons;
    u8  _pad;
};

void ps2_mouse_handler(void);            /* called from IRQ12 path */
int  ps2_mouse_read(struct helix_mouse_event *out, int max);  /* non-blocking */
