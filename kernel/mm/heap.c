#include "helix/heap.h"
#include "helix/pmm.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/types.h"

/*
 * Tiny first-fit heap over a fixed pool of PMM pages.
 * Block header: size | free flag in low bit of size? Keep simple:
 *   [u64 size][payload...]  size includes header; free list intrusive.
 */

struct freenode {
    u64 size;               /* total block bytes including this header */
    struct freenode *next;
};

static u8 *g_heap_base;
static u64 g_heap_size;
static struct freenode *g_free;

#define HEAP_PAGES  2048ull        /* 8 MiB — load BusyBox (~1.1MiB) + dyn ELFs */
#define HDR_SIZE    ((u64)sizeof(struct freenode))
#define MIN_BLOCK   (HDR_SIZE + 16)

int heap_init(void)
{
    u64 phys = pmm_alloc_pages(HEAP_PAGES);
    if (!phys)
        return -1;
    g_heap_base = (u8 *)(uintptr_t)phys;
    g_heap_size = HEAP_PAGES * PAGE_SIZE;
    memset(g_heap_base, 0, (size_t)g_heap_size);

    g_free = (struct freenode *)g_heap_base;
    g_free->size = g_heap_size;
    g_free->next = 0;

    kprintf("[heap] base=0x%llx size=%llu KiB\n",
            (unsigned long long)phys,
            (unsigned long long)(g_heap_size / 1024));
    return 0;
}

static void split_node(struct freenode *n, u64 need)
{
    if (n->size >= need + MIN_BLOCK) {
        struct freenode *rest = (struct freenode *)((u8 *)n + need);
        rest->size = n->size - need;
        rest->next = n->next;
        n->size = need;
        n->next = rest;
    }
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return 0;
    u64 need = align_up_u64((u64)size + 8, 16); /* 8-byte used-header (size only) */
    /* used layout: [u64 size][payload] — freenode reuses first 8 + next when free */
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;

    struct freenode **pp = &g_free;
    while (*pp) {
        struct freenode *n = *pp;
        if (n->size >= need) {
            split_node(n, need);
            *pp = n->next;
            /* mark used: store size, payload after 8 bytes */
            u64 *hdr = (u64 *)n;
            *hdr = n->size; /* full block size */
            return (void *)(hdr + 1);
        }
        pp = &n->next;
    }
    return 0;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    u64 *hdr = (u64 *)ptr - 1;
    u64 sz = *hdr;
    if (sz < MIN_BLOCK || (u8 *)hdr < g_heap_base
        || (u8 *)hdr + sz > g_heap_base + g_heap_size)
        return;

    struct freenode *n = (struct freenode *)hdr;
    n->size = sz;
    n->next = 0;

    /* D5: full coalesce — merge with next AND previous physical neighbors.
     * The heap is small (8 MiB) so linear free-list scans are acceptable. */

    /* --- merge with NEXT physical neighbor if free --- */
    struct freenode *next_phys = (struct freenode *)((u8 *)n + n->size);
    if ((u8 *)next_phys < g_heap_base + g_heap_size) {
        /* Walk free list to see if next_phys is free */
        struct freenode *fp = g_free;
        while (fp) {
            if (fp == next_phys) {
                /* next_phys is free — absorb it */
                n->size += fp->size;
                /* Remove fp from free list */
                struct freenode **pp = &g_free;
                while (*pp && *pp != fp)
                    pp = &(*pp)->next;
                if (*pp == fp)
                    *pp = fp->next;
                break;
            }
            fp = fp->next;
        }
    }

    /* --- merge with PREVIOUS physical neighbor if free --- */
    if ((u8 *)n > g_heap_base) {
        /* Walk free list looking for a block that ends exactly at n */
        struct freenode *fp = g_free;
        while (fp) {
            if ((u8 *)fp + fp->size == (u8 *)n) {
                /* prev_phys is free — absorb n into it */
                fp->size += n->size;
                /* n is now absorbed; don't add n to free list */
                return;
            }
            fp = fp->next;
        }
    }

    /* No backward merge — add n to free list head */
    n->next = g_free;
    g_free = n;
}
