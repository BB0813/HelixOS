/* Dynamic-style hello: ET_DYN-ish linked with INTERP, entry after ld-helix jumps.
 * Built as freestanding with PT_INTERP injected by scripts/elf_set_interp.py
 */
#include "usys.h"

void _start(void)
{
    uwrite("HelloDynOK\n");
    sys_exit(0);
}
