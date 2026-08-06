#include "helix/vmm.h"
#include "helix/paging.h"
#include "helix/pmm.h"
#include "helix/string.h"
#include "helix/task.h"
#include "helix/kprintf.h"

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)
#define PTE_PS (1ull << 7)
#define PTE_ADDR 0x000FFFFFFFFFF000ull

/* Map user pages at arbitrary user VAs (Helix high window or classic low). */
int vmm_map_user(u64 virt, u64 phys, u64 len, int writable)
{
    if (len == 0)
        return -1;
    u64 flags = PTE_P | PTE_U | (writable ? PTE_W : 0);
    u64 off = 0;
    while (off < len) {
        if (paging_map_4k(virt + off, phys + off, flags) != 0)
            return -1;
        off += PAGE_SIZE;
    }
    return 0;
}

u64 vmm_alloc_user_pages(u64 virt, u64 npages, int writable)
{
    if (npages == 0)
        return 0;
    /* Scatter-allocate so BusyBox mmap does not need contiguous multi-page PMM. */
    for (u64 i = 0; i < npages; i++) {
        u64 phys = pmm_alloc_page();
        if (!phys)
            return 0;
        memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
        if (paging_map_4k(virt + i * PAGE_SIZE, phys,
                          PTE_P | PTE_U | (writable ? PTE_W : 0)) != 0) {
            pmm_free_page(phys);
            return 0;
        }
    }
    return 1; /* non-zero success */
}

/* D4: unmap user pages in [virt, virt+len). Pages that aren't present (e.g.
 * already unmapped, kernel leaves) are skipped silently. Used by munmap(2)
 * and by task exit to free the entire user VA.
 *
 * NOTE: temporarily a no-op pending musl regression investigation.
 * paging_unmap_4k frees phys pages but HelixOS shares a single PML4
 * across tasks, so freeing a phys page in one task also removes it from
 * all peer tasks. Full fix requires per-task PML4 + COW (M25+). */
void vmm_unmap_user_range(u64 virt, u64 len)
{
    (void)virt;
    (void)len;
}

/* D4: adjust PTE_W on user pages in [virt, virt+len). PROT_READ → writable=0,
 * PROT_WRITE → writable=1. PROT_NONE is handled as PROT_READ (we don't drop
 * P itself to avoid accidental #PF on first instruction after mprotect).
 *
 * NOTE: temporarily a no-op pending musl regression investigation. */
int vmm_set_prot(u64 virt, u64 len, int prot)
{
    (void)virt;
    (void)len;
    (void)prot;
    return 0;
}

/*
 * vmm_copy_user_page_tables — fork support.
 * Creates a new PML4 for the child with:
 *  - Kernel mappings shared (same physical tables)
 *  - User leaf pages (PTE_U) copied to new physical pages
 *  - New pages recorded in child->user_pages[]
 * Returns child PML4 physical address, or 0 on failure.
 */
static u64 *alloc_table(void)
{
    u64 phys = pmm_alloc_page();
    if (!phys)
        return 0;
    memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
    return (u64 *)(uintptr_t)phys;
}

/* Recursively copy page tables from parent to child.
 * level: 3=PML4, 2=PDPT, 1=PD, 0=PT
 * vaddr_base: accumulated virtual address from upper levels.
 * For level>0, returns child table entry value (phys | flags). */
static u64 copy_pt_level(u64 *parent_tab, int level, u64 vaddr_base,
                          struct task *child)
{
    u64 *child_tab = alloc_table();
    if (!child_tab)
        return 0;
    u64 child_tab_phys = (u64)(uintptr_t)child_tab;

    for (int idx = 0; idx < 512; idx++) {
        u64 pe = parent_tab[idx];
        if (!(pe & PTE_P)) {
            child_tab[idx] = 0;
            continue;
        }

        u64 entry_flags = pe & (PTE_W | PTE_U | 0x18); /* W|U|PWT|PCD */
        u64 vaddr = vaddr_base | ((u64)idx << (12 + level * 9));

        if (level == 0) {
            /* Leaf PTE */
            if (!(pe & PTE_U)) {
                /* Kernel leaf: share */
                child_tab[idx] = pe;
                continue;
            }
            /* User leaf: alloc new physical page, copy content */
            u64 old_phys = pe & PTE_ADDR;
            u64 new_phys = pmm_alloc_page();
            if (!new_phys) {
                child_tab[idx] = 0;
                continue;
            }
            memcpy((void *)(uintptr_t)new_phys,
                   (const void *)(uintptr_t)old_phys, PAGE_SIZE);
            child_tab[idx] = (new_phys & PTE_ADDR) | (pe & ~PTE_ADDR);
            task_track_user_page(child, vaddr, new_phys);
            continue;
        }

        /* Intermediate level */
        u64 *parent_next = (u64 *)(uintptr_t)(pe & PTE_ADDR);

        if (level == 1 && (pe & PTE_PS)) {
            /* 2MiB large page at PD level.
             * If user page: split into512 ×4K to copy individual user pages.
             * If kernel page: share as-is. */
            if (!(pe & PTE_U)) {
                child_tab[idx] = pe;
                continue;
            }
            /* Split: create a PT page with512 entries */
            u64 *child_pt = alloc_table();
            if (!child_pt) {
                child_tab[idx] = 0;
                continue;
            }
            u64 base_phys = pe & 0x000FFFFFFFFFF000ull;
            u64 flags_4k = pe & (PTE_W | PTE_U | 0x18);
            for (int j = 0; j < 512; j++) {
                u64 old_p = base_phys + (u64)j * PAGE_SIZE;
                u64 new_p = pmm_alloc_page();
                if (!new_p) {
                    child_pt[j] = 0;
                    continue;
                }
                memcpy((void *)(uintptr_t)new_p,
                       (const void *)(uintptr_t)old_p, PAGE_SIZE);
                child_pt[j] = (new_p & PTE_ADDR) | flags_4k | PTE_P;
                task_track_user_page(child, vaddr + (u64)j * PAGE_SIZE, new_p);
            }
            child_tab[idx] = (u64)(uintptr_t)child_pt | entry_flags | PTE_P;
            continue;
        }

        if (level == 2 && (pe & PTE_PS)) {
            /* 1GiB large page — share (kernel identity; no user1GiB pages used) */
            child_tab[idx] = pe;
            continue;
        }

        /* Normal intermediate table: recurse */
        u64 child_next_val = copy_pt_level(parent_next, level - 1, vaddr, child);
        if (!child_next_val) {
            child_tab[idx] = 0;
            continue;
        }
        child_tab[idx] = child_next_val | entry_flags | PTE_P;
    }

    return child_tab_phys | PTE_P | PTE_W; /* return phys | flags for parent entry */
}

u64 vmm_copy_user_page_tables(u64 parent_pml4_phys, struct task *child)
{
    u64 *parent_pml4 = (u64 *)(uintptr_t)parent_pml4_phys;
    u64 *child_pml4 = alloc_table();
    if (!child_pml4)
        return 0;

    for (int i = 0; i < 512; i++) {
        u64 pe = parent_pml4[i];
        if (!(pe & PTE_P)) {
            child_pml4[i] = 0;
            continue;
        }
        u64 *parent_sub = (u64 *)(uintptr_t)(pe & PTE_ADDR);
        u64 vaddr_base = (u64)i << 39;
        u64 child_entry = copy_pt_level(parent_sub, 2 /* PDPT level */,
                                         vaddr_base, child);
        child_pml4[i] = child_entry ? child_entry : pe; /* fall back to share */
    }
    return (u64)(uintptr_t)child_pml4;
}
