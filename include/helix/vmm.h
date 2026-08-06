#pragma once

#include "helix/types.h"

struct task; /* forward declaration for vmm_copy_user_page_tables */

/* Map `len` bytes of physical `phys` at user VA `virt` with U|W|P (4K pages).
 * virt must be in [USER_BASE, USER_STACK_TOP). Returns 0 on success. */
int  vmm_map_user(u64 virt, u64 phys, u64 len, int writable);
/* Allocate and zero `npages` physical pages, map at virt. Returns phys base or 0. */
u64  vmm_alloc_user_pages(u64 virt, u64 npages, int writable);
void vmm_unmap_user_range(u64 virt, u64 len); /* D4: real impl; frees user phys pages */
/* D4: adjust PTE_W on user pages in [virt, virt+len). prot follows Linux
 * PROT_* bits (PROT_READ=1, PROT_WRITE=2). PROT_NONE treated as PROT_READ. */
int  vmm_set_prot(u64 virt, u64 len, int prot);
/* Copy parent's user page tables into a new PML4; child owns copies of user pages. */
u64  vmm_copy_user_page_tables(u64 parent_pml4_phys, struct task *child);
