#pragma once

#include "helix/types.h"

/* Programmable Interval Timer channel 0 → PIC IRQ0. */
#define PIT_HZ          100u

void pit_init(u32 hz);
