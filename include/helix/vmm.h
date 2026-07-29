#pragma once

#include "helix/types.h"

/* Map `len` bytes of physical `phys` at user VA `virt` with U|W|P (4K pages).
 * virt must be in [USER_BASE, USER_STACK_TOP). Returns 0 on success. */
int  vmm_map_user(u64 virt, u64 phys, u64 len, int writable);
/* Allocate and zero `npages` physical pages, map at virt. Returns phys base or 0. */
u64  vmm_alloc_user_pages(u64 virt, u64 npages, int writable);
void vmm_unmap_user_range(u64 virt, u64 len); /* best-effort; M3 may no-op free */
