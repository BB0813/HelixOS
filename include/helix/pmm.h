#pragma once

#include "helix/boot_info.h"
#include "helix/types.h"

/* Physical page allocator (bitmap, 4K granularity). */

int  pmm_init(struct helix_boot_info *info);
/* Permanently mark [phys_start, phys_end) used (e.g. classic user ELF hole). */
void pmm_reserve(u64 phys_start, u64 phys_end);
u64  pmm_alloc_page(void);           /* returns phys addr, or 0 on OOM */
void pmm_free_page(u64 phys);
u64  pmm_alloc_pages(u64 n);         /* contiguous n pages; 0 on fail */
void pmm_free_pages(u64 phys, u64 n);
u64  pmm_total_pages(void);
u64  pmm_free_pages_count(void);
u64  pmm_phys_ceiling(void);
/* D4.2: per-page refcounts for COW + safe unmap.
 * pmm_page_own: mark phys as a new private mapping (refs=1).
 * pmm_page_share: increment (fork COW share).
 * pmm_page_deref: decrement; free at 0. refs==0 (reserved / MMIO) → no-op,
 *   never frees a page the kernel doesn't own. */
void pmm_page_own(u64 phys);
void pmm_page_share(u64 phys);
void pmm_page_deref(u64 phys);
u32  pmm_page_refcount(u64 phys);
