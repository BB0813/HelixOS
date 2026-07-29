#include "usys.h"

/* task2 — second cooperative user task (linked at different VA) */
void _start(void)
{
    uwrite("task2: hi\n");
    sys_yield();
    uwrite("task2: after yield\n");
    sys_yield();
    uwrite("task2: bye\n");
    sys_exit(0);
}
