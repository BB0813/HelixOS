#pragma once

#include "helix/types.h"

void idt_init(void);
/* Install handler for vector (0..255). Used for IRQ gates after pic remap. */
void idt_set_gate(int vec, void (*handler)(void));
