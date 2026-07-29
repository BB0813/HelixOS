#include "helix/vmm.h"
#include "helix/paging.h"
#include "helix/pmm.h"
#include "helix/string.h"

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)

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

void vmm_unmap_user_range(u64 virt, u64 len)
{
    (void)virt;
    (void)len;
}
