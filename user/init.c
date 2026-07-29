#include "usys.h"

/* init — first user process */
void _start(void)
{
    uwrite("Hello from Ring3\n");
    uwrite("init: online\n");
    sys_yield();
    uwrite("init: after yield\n");
    sys_yield();
    uwrite("init: exiting\n");
    sys_exit(0);
}
