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
/* The boot identity-map template (never holds user pages). Per-task PML4s clone it. */
u64  paging_kernel_pml4(void);
/* Switch the kernel's active page tables to a new CR3/PML4. */
void paging_set_pml4(u64 pml4);

/* Map a single 4K page at virt → phys with flags (PTE bits). May split 2MiB. */
int  paging_map_4k(u64 virt, u64 phys, u64 flags);
int  paging_map_mmio(u64 phys, u64 len);
int  paging_set_user_range(u64 va, u64 len);
/* Walk current CR3: phys of page containing virt, or 0 if unmapped. */
u64  paging_virt_to_phys(u64 virt);
/* D4: unmap a single 4K page. Returns 0 if it was a user leaf (and releases the
 * underlying phys page via pmm_page_deref); -1 otherwise. Kernel leaves are
 * never touched. */
int  paging_unmap_4k(u64 virt);
/* D4: walk current CR3 and adjust W bit on user leaves in [virt, virt+len).
 * PROT_NONE keeps P|U; PROT_READ drops W; PROT_READ|PROT_WRITE sets W. */
int  paging_set_prot_range(u64 virt, u64 len, int writable);
