#include "helix/paging.h"
#include "helix/pmm.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/panic.h"

/* 4-level identity map using 2MiB large pages for low RAM.
 * User 4K maps installed later via paging_map_4k on the same CR3.
 */

#define PTE_P   (1ull << 0)
#define PTE_W   (1ull << 1)
#define PTE_U   (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_PS  (1ull << 7)

static u64 g_mapped_ceiling;
static u64 g_cr3;
static u64 *g_pml4;

static u64 *alloc_table(void)
{
    u64 phys = pmm_alloc_page();
    if (!phys)
        return 0;
    void *p = (void *)(uintptr_t)phys;
    memset(p, 0, PAGE_SIZE);
    return (u64 *)p;
}

int paging_init_identity(u64 phys_ceiling)
{
    u64 ceil = align_up_u64(phys_ceiling, LARGE_PAGE_SIZE);
    if (ceil < LARGE_PAGE_SIZE)
        ceil = LARGE_PAGE_SIZE;

    g_pml4 = alloc_table();
    if (!g_pml4)
        return -1;

    u64 n_large = ceil / LARGE_PAGE_SIZE;
    u64 n_pd = (n_large + 511) / 512;
    if (n_pd == 0)
        n_pd = 1;
    if (n_pd > 512) {
        kprintf("[paging] ceiling too high (%llu PD needed); capping to 512GiB\n",
                (unsigned long long)n_pd);
        n_pd = 512;
        n_large = n_pd * 512;
        ceil = n_large * LARGE_PAGE_SIZE;
    }

    u64 *pdpt = alloc_table();
    if (!pdpt)
        return -1;
    g_pml4[0] = (u64)(uintptr_t)pdpt | PTE_P | PTE_W;

    u64 large_idx = 0;
    for (u64 pdi = 0; pdi < n_pd; pdi++) {
        u64 *pd = alloc_table();
        if (!pd)
            return -1;
        pdpt[pdi] = (u64)(uintptr_t)pd | PTE_P | PTE_W;
        for (u64 i = 0; i < 512 && large_idx < n_large; i++, large_idx++) {
            u64 phys = large_idx * LARGE_PAGE_SIZE;
            pd[i] = phys | PTE_P | PTE_W | PTE_PS;
        }
    }

    g_cr3 = (u64)(uintptr_t)g_pml4;
    __asm__ volatile("mov %0, %%cr3" : : "r"(g_cr3) : "memory");

    g_mapped_ceiling = ceil;
    kprintf("[paging] identity 0..0x%llx via 2MiB pages, CR3=0x%llx (%llu PD)\n",
            (unsigned long long)ceil,
            (unsigned long long)g_cr3,
            (unsigned long long)n_pd);
    return 0;
}

void paging_invlpg(u64 va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

u64 paging_mapped_ceiling(void)
{
    return g_mapped_ceiling;
}

u64 paging_cr3(void)
{
    return g_cr3;
}

/* Walk/create 4-level tables and install a 4K PTE. Kernel identity stays on 2MiB.
 * Intermediate entries only get U if the leaf is user-accessible — never force U
 * on kernel MMIO paths (that used to taint PML4[0] and confuse later walks). */
int paging_map_4k(u64 virt, u64 phys, u64 flags)
{
    if (!g_pml4)
        return -1;
    if (virt & (PAGE_SIZE - 1) || phys & (PAGE_SIZE - 1))
        return -1;

    u64 user = (flags & PTE_U) ? PTE_U : 0;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;
    u64 i1 = (virt >> 12) & 0x1FF;

    if (!(g_pml4[i4] & PTE_P)) {
        u64 *t = alloc_table();
        if (!t)
            return -1;
        g_pml4[i4] = (u64)(uintptr_t)t | PTE_P | PTE_W | user;
    } else if (user) {
        g_pml4[i4] |= PTE_U;
    }
    u64 *pdpt = (u64 *)(uintptr_t)(g_pml4[i4] & ~0xFFFull);

    if (!(pdpt[i3] & PTE_P)) {
        u64 *t = alloc_table();
        if (!t)
            return -1;
        pdpt[i3] = (u64)(uintptr_t)t | PTE_P | PTE_W | user;
    } else if (pdpt[i3] & PTE_PS) {
        return -1; /* 1GiB page in the way */
    } else if (user) {
        pdpt[i3] |= PTE_U;
    }
    u64 *pd = (u64 *)(uintptr_t)(pdpt[i3] & ~0xFFFull);

    if (pd[i2] & PTE_P) {
        if (pd[i2] & PTE_PS) {
            /* Split 2MiB identity leaf into 512×4K so we can remap one page
             * without destroying the rest of the large mapping. */
            u64 big = pd[i2];
            u64 big_phys = big & 0x000FFFFFFFFFF000ull;
            u64 big_flags = big & 0x80000000000001EFull; /* NX + PWT/PCD/U/W/P etc, drop PS */
            big_flags &= ~PTE_PS;
            u64 *t = alloc_table();
            if (!t)
                return -1;
            for (u64 j = 0; j < 512; j++) {
                t[j] = (big_phys + j * PAGE_SIZE) | big_flags | PTE_P;
            }
            pd[i2] = (u64)(uintptr_t)t | PTE_P | PTE_W | (big_flags & PTE_U) | user;
            /* Shoot down the whole 2MiB range */
            for (u64 j = 0; j < 512; j++)
                paging_invlpg((virt & ~((1ull << 21) - 1)) + j * PAGE_SIZE);
        } else if (user) {
            pd[i2] |= PTE_U;
        }
    } else {
        u64 *t = alloc_table();
        if (!t)
            return -1;
        pd[i2] = (u64)(uintptr_t)t | PTE_P | PTE_W | user;
    }
    u64 *pt = (u64 *)(uintptr_t)(pd[i2] & ~0xFFFull);

    /* phys addr bits + allowed flag bits (incl NX); never raw & 0xFFF only. */
    pt[i1] = (phys & 0x000FFFFFFFFFF000ull) | (flags & 0x8000000000000FFFull) | PTE_P;
    paging_invlpg(virt);
    return 0;
}

int paging_map_mmio(u64 phys, u64 len)
{
    u64 start = align_down_u64(phys, PAGE_SIZE);
    u64 end = align_up_u64(phys + len, PAGE_SIZE);
    /* Kernel-only UC-ish mapping — no PTE_U on leaf or parents. */
    u64 flags = PTE_P | PTE_W | PTE_PCD | PTE_PWT;
    for (u64 va = start; va < end; va += PAGE_SIZE) {
        if (paging_map_4k(va, va, flags) != 0)
            return -1;
    }
    return 0;
}

/* Walk existing identity map and set U on leaves covering [va,va+len). */
int paging_set_user_range(u64 va, u64 len)
{
    if (!g_pml4 || len == 0)
        return -1;
    u64 start = align_down_u64(va, PAGE_SIZE);
    u64 end = align_up_u64(va + len, PAGE_SIZE);
    for (u64 v = start; v < end; ) {
        u64 i4 = (v >> 39) & 0x1FF;
        u64 i3 = (v >> 30) & 0x1FF;
        u64 i2 = (v >> 21) & 0x1FF;
        u64 i1 = (v >> 12) & 0x1FF;
        if (!(g_pml4[i4] & PTE_P))
            return -1;
        g_pml4[i4] |= PTE_U;
        u64 *pdpt = (u64 *)(uintptr_t)(g_pml4[i4] & ~0xFFFull);
        if (!(pdpt[i3] & PTE_P))
            return -1;
        pdpt[i3] |= PTE_U;
        if (pdpt[i3] & PTE_PS) {
            pdpt[i3] |= PTE_U;
            v = align_up_u64(v + 1, 1ull << 30);
            continue;
        }
        u64 *pd = (u64 *)(uintptr_t)(pdpt[i3] & ~0xFFFull);
        if (!(pd[i2] & PTE_P))
            return -1;
        if (pd[i2] & PTE_PS) {
            pd[i2] |= PTE_U;
            paging_invlpg(v);
            v += LARGE_PAGE_SIZE;
            continue;
        }
        pd[i2] |= PTE_U;
        u64 *pt = (u64 *)(uintptr_t)(pd[i2] & ~0xFFFull);
        if (!(pt[i1] & PTE_P))
            return -1;
        pt[i1] |= PTE_U;
        paging_invlpg(v);
        v += PAGE_SIZE;
    }
    return 0;
}

u64 paging_virt_to_phys(u64 virt)
{
    if (!g_pml4)
        return 0;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;
    u64 i1 = (virt >> 12) & 0x1FF;
    if (!(g_pml4[i4] & PTE_P))
        return 0;
    u64 *pdpt = (u64 *)(uintptr_t)(g_pml4[i4] & ~0xFFFull);
    if (!(pdpt[i3] & PTE_P))
        return 0;
    if (pdpt[i3] & PTE_PS)
        return (pdpt[i3] & 0x000FFFFFC0000000ull) | (virt & 0x3FFFFFFFull);
    u64 *pd = (u64 *)(uintptr_t)(pdpt[i3] & ~0xFFFull);
    if (!(pd[i2] & PTE_P))
        return 0;
    if (pd[i2] & PTE_PS)
        return (pd[i2] & 0x000FFFFFFFE00000ull) | (virt & 0x1FFFFFull);
    u64 *pt = (u64 *)(uintptr_t)(pd[i2] & ~0xFFFull);
    if (!(pt[i1] & PTE_P))
        return 0;
    return (pt[i1] & 0x000FFFFFFFFFF000ull) | (virt & 0xFFFull);
}
