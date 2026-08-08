#include "helix/pmm.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/panic.h"

/* EFI memory types we treat as free after ExitBootServices. */
enum {
    EFI_ConventionalMemory  = 7,
    EFI_BootServicesCode    = 3,
    EFI_BootServicesData    = 4,
    EFI_LoaderCode          = 1,
    EFI_LoaderData          = 2,
    EFI_ACPIReclaimMemory   = 9,
};

static u8  *g_bitmap;
static u8  *g_refs;          /* per-page refcounts (1 byte/page); 0 = reserved/unowned */
static u64  g_total_pages;   /* pages covered by bitmap (from 0..ceiling) */
static u64  g_free_pages;
static u64  g_ceiling;       /* exclusive phys end */

static inline int bm_test(u64 page)
{
    return (g_bitmap[page >> 3] >> (page & 7)) & 1;
}

static inline void bm_set(u64 page)
{
    g_bitmap[page >> 3] |= (u8)(1u << (page & 7));
}

static inline void bm_clear(u64 page)
{
    g_bitmap[page >> 3] &= (u8)~(1u << (page & 7));
}

/* M1: only ConventionalMemory is free.
 * BootServices* may still hold the UEFI stack until we switch (and we keep it).
 * Loader* holds our image, boot_info, mmap copy — must not be reclaimed yet.
 */
static int is_free_type(u32 type)
{
    return type == EFI_ConventionalMemory;
}

/* Reserve [start, end) pages as used. */
static void reserve_range(u64 phys_start, u64 phys_end)
{
    u64 p0 = phys_start >> PAGE_SHIFT;
    u64 p1 = (phys_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (p1 > g_total_pages)
        p1 = g_total_pages;
    for (u64 p = p0; p < p1; p++) {
        if (!bm_test(p)) {
            bm_set(p);
            if (g_free_pages)
                g_free_pages--;
        }
    }
}

void pmm_reserve(u64 phys_start, u64 phys_end)
{
    if (!g_bitmap || phys_end <= phys_start)
        return;
    reserve_range(phys_start, phys_end);
    kprintf("[pmm] reserved phys 0x%llx..0x%llx\n",
            (unsigned long long)phys_start, (unsigned long long)phys_end);
}

int pmm_init(struct helix_boot_info *info)
{
    if (!info || !info->mmap || info->mmap_count == 0)
        return -1;

    g_ceiling = align_up_u64(info->phys_ceiling, PAGE_SIZE);
    if (g_ceiling < 16u * 1024u * 1024u)
        g_ceiling = 16u * 1024u * 1024u; /* safety floor */

    g_total_pages = g_ceiling >> PAGE_SHIFT;
    u64 bitmap_bytes = (g_total_pages + 7) / 8;
    /* D4.2: per-page refcounts (1 byte/page) placed after the bitmap. */
    u64 refs_bytes = g_total_pages;
    u64 bitmap_pages = (align_up_u64(bitmap_bytes, PAGE_SIZE) +
                        align_up_u64(refs_bytes, PAGE_SIZE) + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Find a ConventionalMemory region large enough for the bitmap + refs.
     * Prefer above 1MiB. We cannot yet allocate — place bitmap by scanning map. */
    u64 bitmap_phys = 0;
    for (u64 i = 0; i < info->mmap_count; i++) {
        struct helix_mmap_entry *e = &info->mmap[i];
        if (e->type != EFI_ConventionalMemory)
            continue;
        u64 start = e->phys_start;
        u64 end   = e->phys_start + e->npages * PAGE_SIZE;
        if (start < 0x100000)
            start = 0x100000;
        start = align_up_u64(start, PAGE_SIZE);
        if (end > start && (end - start) >= bitmap_pages * PAGE_SIZE) {
            bitmap_phys = start;
            break;
        }
    }
    if (!bitmap_phys)
        return -1;

    g_bitmap = (u8 *)(uintptr_t)bitmap_phys;
    memset(g_bitmap, 0xFF, (size_t)bitmap_bytes); /* all used initially */
    g_refs = (u8 *)(uintptr_t)(bitmap_phys + align_up_u64(bitmap_bytes, PAGE_SIZE));
    memset(g_refs, 0, refs_bytes);                /* all unowned initially */
    g_free_pages = 0;

    /* Mark free types as free */
    for (u64 i = 0; i < info->mmap_count; i++) {
        struct helix_mmap_entry *e = &info->mmap[i];
        if (!is_free_type(e->type))
            continue;
        u64 start = e->phys_start;
        u64 end   = e->phys_start + e->npages * PAGE_SIZE;
        u64 p0 = start >> PAGE_SHIFT;
        u64 p1 = (end + PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (p1 > g_total_pages)
            p1 = g_total_pages;
        for (u64 p = p0; p < p1; p++) {
            if (bm_test(p)) {
                bm_clear(p);
                g_free_pages++;
            }
        }
    }

    /* Always reserve low 1MiB (IVT/BIOS/etc residual) */
    reserve_range(0, 0x100000);
    /* Reserve bitmap storage */
    reserve_range(bitmap_phys, bitmap_phys + bitmap_pages * PAGE_SIZE);
    /* Reserve boot_info mmap array pages if provided as loader data — covered by
     * not freeing non-conventional carefully; also reserve image if known. */
    if (info->image_base && info->image_size)
        reserve_range(info->image_base, info->image_base + info->image_size);
    /* Reserve the boot_info mmap buffer itself */
    reserve_range((u64)(uintptr_t)info->mmap,
                  (u64)(uintptr_t)info->mmap + info->mmap_count * sizeof(struct helix_mmap_entry));
    reserve_range((u64)(uintptr_t)info,
                  (u64)(uintptr_t)info + sizeof(*info));

    kprintf("[pmm] ceiling=0x%llx pages=%llu free=%llu bitmap@0x%llx\n",
            (unsigned long long)g_ceiling,
            (unsigned long long)g_total_pages,
            (unsigned long long)g_free_pages,
            (unsigned long long)bitmap_phys);
    return 0;
}

u64 pmm_alloc_page(void)
{
    for (u64 p = 0; p < g_total_pages; p++) {
        if (!bm_test(p)) {
            bm_set(p);
            g_free_pages--;
            g_refs[p] = 1;
            return p << PAGE_SHIFT;
        }
    }
    return 0;
}

void pmm_free_page(u64 phys)
{
    u64 p = phys >> PAGE_SHIFT;
    if (p >= g_total_pages)
        return;
    if (!bm_test(p))
        return; /* double free ignore */
    bm_clear(p);
    g_free_pages++;
}

u64 pmm_alloc_pages(u64 n)
{
    if (n == 0)
        return 0;
    if (n == 1)
        return pmm_alloc_page();
    for (u64 p = 0; p + n <= g_total_pages; p++) {
        u64 ok = 1;
        for (u64 i = 0; i < n; i++) {
            if (bm_test(p + i)) {
                ok = 0;
                p += i;
                break;
            }
        }
        if (!ok)
            continue;
        for (u64 i = 0; i < n; i++) {
            bm_set(p + i);
            g_free_pages--;
            g_refs[p + i] = 1;
        }
        return p << PAGE_SHIFT;
    }
    return 0;
}

void pmm_free_pages(u64 phys, u64 n)
{
    for (u64 i = 0; i < n; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

u64 pmm_total_pages(void) { return g_total_pages; }
u64 pmm_free_pages_count(void) { return g_free_pages; }
u64 pmm_phys_ceiling(void) { return g_ceiling; }

/* D4.2: per-page refcounts. A fresh allocation is owned (refs=1). refs==0 means the
 * page is reserved / MMIO / a shared kernel leaf — deref never frees those. */
void pmm_page_own(u64 phys)
{
    u64 p = phys >> PAGE_SHIFT;
    if (p < g_total_pages)
        g_refs[p] = 1;
}

void pmm_page_share(u64 phys)
{
    u64 p = phys >> PAGE_SHIFT;
    if (p < g_total_pages && g_refs[p] < 0xFF)
        g_refs[p]++;
}

void pmm_page_deref(u64 phys)
{
    u64 p = phys >> PAGE_SHIFT;
    if (p >= g_total_pages)
        return;
    if (g_refs[p] == 0)
        return; /* reserved / MMIO / kernel leaf — never free */
    if (--g_refs[p] == 0)
        pmm_free_page(phys);
}

u32 pmm_page_refcount(u64 phys)
{
    u64 p = phys >> PAGE_SHIFT;
    return (p < g_total_pages) ? (u32)g_refs[p] : 0;
}
