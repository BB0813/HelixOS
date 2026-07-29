#include "helix/pit.h"
#include "helix/cpuio.h"
#include "helix/kprintf.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_INPUT   1193182u

void pit_init(u32 hz)
{
    if (hz == 0)
        hz = PIT_HZ;
    if (hz > 10000)
        hz = 10000;

    u32 div = PIT_INPUT / hz;
    if (div == 0)
        div = 1;

    /* channel 0, lobyte/hibyte, mode 3 (square wave), binary */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)((div >> 8) & 0xFF));

    kprintf("[pit] ch0 mode3 ~%u Hz (div=%u)\n", (unsigned)hz, (unsigned)div);
}
