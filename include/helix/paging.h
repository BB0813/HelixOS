#pragma once

#include "helix/types.h"

/* PTE flags — shared with paging.c */
#define PTE_P   (1ull << 0)
#define PTE_W   (1ull << 1)
#define PTE_U   (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_PS  (1ull << 7)

/* Identity-map [0, phys_ceiling) with 2MiB pages; load CR3. */
int  paging_init_identity(u64 phys_ceiling);
void paging_invlpg(u64 va);
u64  paging_mapped_ceiling(void);
u64  paging_cr3(void);

/* Map a single 4K page at virt → phys with flags (PTE bits). May split 2MiB. */
int  paging_map_4k(u64 virt, u64 phys, u64 flags);
int  paging_map_mmio(u64 phys, u64 len);
int  paging_set_user_range(u64 va, u64 len);
/* Walk current CR3: phys of page containing virt, or 0 if unmapped. */
u64  paging_virt_to_phys(u64 virt);
