#include "helix/elf.h"
#include "helix/vmm.h"
#include "helix/pmm.h"
#include "helix/paging.h"
#include "helix/syscall.h"
#include "helix/vfs.h"
#include "helix/heap.h"
#include "helix/string.h"
#include "helix/kprintf.h"
#include "helix/types.h"

#define EI_NIDENT 16
#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62
#define PT_LOAD   1
#define PT_INTERP 3
#define PT_PHDR   6

struct elf64_ehdr {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed));

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)

static int ensure_user_page_high(u64 virt)
{
    enum { WIN = 0x4000000ull / PAGE_SIZE / 8 };
    static u8 mapped[WIN];
    if (virt < USER_BASE || virt >= USER_STACK_TOP)
        return -1;
    u64 off = virt - USER_BASE;
    u64 pg = off >> PAGE_SHIFT;
    u64 bi = pg >> 3;
    u8  mask = (u8)(1u << (pg & 7));
    if (bi >= sizeof(mapped))
        return -1;
    if (mapped[bi] & mask)
        return 0;
    u64 phys = pmm_alloc_page();
    if (!phys)
        return -1;
    memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
    if (paging_map_4k(virt, phys, PTE_P | PTE_W | PTE_U) != 0) {
        pmm_free_page(phys);
        return -1;
    }
    mapped[bi] |= mask;
    return 0;
}

/* Low classic ET_EXEC (BusyBox @0x400000): use VA=PA identity.
 * Phys [0x400000,0xA00000) is pmm_reserve'd so kernel heap never steals it. */
/* [1MiB, 10MiB) covers BusyBox LOADs (data BSS ends ~0x912000). */
enum { LOW_SPAN = 0x900000ull };
enum { LOW_BASE = 0x100000ull };
static u8 g_low_mapped[LOW_SPAN / PAGE_SIZE / 8];

static void low_user_map_reset(void)
{
    memset(g_low_mapped, 0, sizeof(g_low_mapped));
}

static int ensure_user_page_low(u64 virt)
{
    if (virt < USER_LOW_MIN || virt >= USER_LOW_MAX)
        return -1;
    if (virt & (PAGE_SIZE - 1))
        return -1;

    if (paging_set_user_range(virt, PAGE_SIZE) == 0) {
        if (virt >= LOW_BASE && virt < LOW_BASE + LOW_SPAN) {
            u64 off = virt - LOW_BASE;
            u64 pg = off >> PAGE_SHIFT;
            g_low_mapped[pg >> 3] |= (u8)(1u << (pg & 7));
        }
        return 0;
    }
    if (paging_map_4k(virt, virt, PTE_P | PTE_W | PTE_U) != 0)
        return -1;
    return 0;
}

static int ensure_user_page(u64 virt)
{
    if (virt >= USER_BASE && virt < USER_STACK_TOP)
        return ensure_user_page_high(virt);
    if (virt >= 0x100000ull && virt < USER_LOW_MAX)
        return ensure_user_page_low(virt);
    /* Allow ld-helix base 0x50000000 region inside high window already.
     * Also allow 0x50000000 explicitly via high check if USER_BASE is 0x40000000
     * and TOP is 0x44000000 — 0x50000000 is OUTSIDE. Map as high-style anon. */
    if (virt >= 0x50000000ull && virt < 0x51000000ull) {
        /* separate small bitmap for interp window 16MiB */
        static u8 im[0x1000000ull / PAGE_SIZE / 8];
        u64 off = virt - 0x50000000ull;
        u64 pg = off >> PAGE_SHIFT;
        u64 bi = pg >> 3;
        u8 mask = (u8)(1u << (pg & 7));
        if (bi >= sizeof(im))
            return -1;
        if (im[bi] & mask)
            return 0;
        u64 phys = pmm_alloc_page();
        if (!phys)
            return -1;
        memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
        if (paging_map_4k(virt, phys, PTE_P | PTE_W | PTE_U) != 0) {
            pmm_free_page(phys);
            return -1;
        }
        im[bi] |= mask;
        return 0;
    }
    return -1;
}

static int map_segment(u64 vaddr, u64 memsz, const u8 *img, u64 img_size,
                       u64 p_offset, u64 filesz)
{
    if (vaddr + memsz < vaddr)
        return -1;
    u64 seg_begin = align_down_u64(vaddr, PAGE_SIZE);
    u64 seg_end = align_up_u64(vaddr + memsz, PAGE_SIZE);
    for (u64 va = seg_begin; va < seg_end; va += PAGE_SIZE) {
        if (ensure_user_page(va) != 0) {
            kprintf("[elf] map fail va=0x%llx\n", (unsigned long long)va);
            return -1;
        }
    }
    /* Copy file bytes. For classic low VA=PA, write through identity.
     * For high remapped pages, write through phys from page walk. */
    if (filesz) {
        if (p_offset + filesz > img_size)
            return -1;
        u64 copied = 0;
        while (copied < filesz) {
            u64 va = vaddr + copied;
            u64 page = align_down_u64(va, PAGE_SIZE);
            u64 off = va - page;
            u64 chunk = PAGE_SIZE - off;
            if (chunk > filesz - copied)
                chunk = filesz - copied;
            u64 phys = paging_virt_to_phys(page);
            u64 dst = phys ? (phys + off) : (va);
            memcpy((void *)(uintptr_t)dst, img + p_offset + copied, (size_t)chunk);
            copied += chunk;
        }
    }
    if (memsz > filesz) {
        u64 zb = vaddr + filesz;
        u64 zlen = memsz - filesz;
        u64 done = 0;
        while (done < zlen) {
            u64 va = zb + done;
            u64 page = align_down_u64(va, PAGE_SIZE);
            u64 off = va - page;
            u64 chunk = PAGE_SIZE - off;
            if (chunk > zlen - done)
                chunk = zlen - done;
            u64 phys = paging_virt_to_phys(page);
            u64 dst = phys ? (phys + off) : va;
            memset((void *)(uintptr_t)dst, 0, (size_t)chunk);
            done += chunk;
        }
    }
    return 0;
}

static int load_loads(const u8 *img, u64 size, const struct elf64_ehdr *eh,
                      u64 bias, u64 *out_base, u64 *out_end, u64 *out_phdr_va)
{
    u64 base = ~0ull, end = 0;
    u64 phdr_va = 0;
    for (u16 i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(img + eh->e_phoff + (u64)i * eh->e_phentsize);
        if (ph->p_type == PT_PHDR) {
            phdr_va = ph->p_vaddr + bias;
            continue;
        }
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        u64 va = ph->p_vaddr + bias;
        if (map_segment(va, ph->p_memsz, img, size, ph->p_offset, ph->p_filesz) != 0)
            return -1;
        if (va < base)
            base = va;
        if (va + ph->p_memsz > end)
            end = va + ph->p_memsz;
    }
    if (base == ~0ull)
        return -1;
    /* If no PT_PHDR, synthesize phdr addr as file-relative at first load + e_phoff
     * only works if phdrs fall in a LOAD — for our demos set from first load base. */
    if (!phdr_va && eh->e_phoff) {
        /* common: phdrs in first page of file mapped at load_base */
        phdr_va = base + eh->e_phoff; /* best-effort for continuous file map */
    }
    *out_base = base;
    *out_end = end;
    *out_phdr_va = phdr_va;
    return 0;
}

static int parse_ehdr(const void *image, u64 size, const struct elf64_ehdr **eh_out)
{
    if (!image || size < sizeof(struct elf64_ehdr))
        return -1;
    const struct elf64_ehdr *eh = image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return -1;
    if (eh->e_ident[4] != 2 || eh->e_machine != EM_X86_64)
        return -1;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return -1;
    if (!eh->e_phoff || !eh->e_phnum)
        return -1;
    if (eh->e_phoff + (u64)eh->e_phnum * eh->e_phentsize > size)
        return -1;
    *eh_out = eh;
    return 0;
}

static const char *find_interp(const u8 *img, u64 size, const struct elf64_ehdr *eh)
{
    for (u16 i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(img + eh->e_phoff + (u64)i * eh->e_phentsize);
        if (ph->p_type != PT_INTERP)
            continue;
        if (ph->p_offset + ph->p_filesz > size || ph->p_filesz == 0)
            return 0;
        return (const char *)(img + ph->p_offset);
    }
    return 0;
}

static int read_file(const char *path, void **out, u64 *out_sz)
{
    struct vfs_file *f = 0;
    if (vfs_open(path, &f) != 0)
        return -1;
    u64 sz = f->size;
    if (sz == 0 || sz > 4 * 1024 * 1024) {
        vfs_close(f);
        return -1;
    }
    void *buf = kmalloc((size_t)sz);
    if (!buf) {
        vfs_close(f);
        return -1;
    }
    u64 n = 0;
    if (vfs_read(f, buf, sz, &n) != 0 || n != sz) {
        kfree(buf);
        vfs_close(f);
        return -1;
    }
    vfs_close(f);
    *out = buf;
    *out_sz = sz;
    return 0;
}

int elf_load_image(const void *image, u64 size, struct elf_load_info *out)
{
    const struct elf64_ehdr *eh;
    if (parse_ehdr(image, size, &eh) != 0 || !out)
        return -1;
    if (find_interp(image, size, eh)) {
        kprintf("[elf] image has PT_INTERP — use dynamic loader\n");
        return -1;
    }
    low_user_map_reset();
    u64 base, end, phdr_va;
    if (load_loads(image, size, eh, 0, &base, &end, &phdr_va) != 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->entry = eh->e_entry;
    out->main_entry = eh->e_entry;
    out->load_base = base;
    out->load_end = end;
    out->phnum = eh->e_phnum;
    out->phentsize = eh->e_phentsize;
    out->phdr_addr = phdr_va ? phdr_va : base + eh->e_phoff;
    out->is_dyn = 0;
    kprintf("[elf] static entry=0x%llx range=0x%llx..0x%llx\n",
            (unsigned long long)out->entry,
            (unsigned long long)base,
            (unsigned long long)end);
    /* Sanity: first instruction bytes at entry must be present (BusyBox debug). */
    {
        u64 ep = out->entry & ~0xFFFull;
        u64 phys = paging_virt_to_phys(ep);
        u32 word = 0;
        if (phys)
            word = *(volatile u32 *)(uintptr_t)(phys + (out->entry & 0xFFF));
        else
            word = *(volatile u32 *)(uintptr_t)out->entry;
        kprintf("[elf] entry bytes @0x%llx = 0x%x (phys=0x%llx)\n",
                (unsigned long long)out->entry, word, (unsigned long long)phys);
    }
    return 0;
}

int elf_load_dynamic(const void *main_img, u64 main_size, struct elf_load_info *out)
{
    const struct elf64_ehdr *eh;
    if (parse_ehdr(main_img, main_size, &eh) != 0 || !out)
        return -1;
    const char *interp = find_interp(main_img, main_size, eh);
    if (!interp) {
        return elf_load_image(main_img, main_size, out);
    }
    kprintf("[elf] PT_INTERP \"%s\"\n", interp);

    void *interp_img = 0;
    u64 interp_sz = 0;
    if (read_file(interp, &interp_img, &interp_sz) != 0) {
        kprintf("[elf] cannot open interp %s\n", interp);
        return -1;
    }
    const struct elf64_ehdr *ieh;
    if (parse_ehdr(interp_img, interp_sz, &ieh) != 0) {
        kfree(interp_img);
        return -1;
    }

    /* PIE (ET_DYN with low vaddrs) needs a load bias; fixed-addr demos keep 0. */
    u64 m_bias = 0, i_bias = 0;
    {
        u64 mlo = ~0ull, ilo = ~0ull;
        for (u16 i = 0; i < eh->e_phnum; i++) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)((const u8 *)main_img + eh->e_phoff +
                                           (u64)i * eh->e_phentsize);
            if (ph->p_type == PT_LOAD && ph->p_memsz && ph->p_vaddr < mlo)
                mlo = ph->p_vaddr;
        }
        for (u16 i = 0; i < ieh->e_phnum; i++) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)((const u8 *)interp_img + ieh->e_phoff +
                                           (u64)i * ieh->e_phentsize);
            if (ph->p_type == PT_LOAD && ph->p_memsz && ph->p_vaddr < ilo)
                ilo = ph->p_vaddr;
        }
        /* Low-linked PIE → place main at USER_BASE, interp at 0x50000000 */
        if (mlo < 0x100000ull)
            m_bias = USER_BASE;
        if (ilo < 0x100000ull)
            i_bias = 0x50000000ull;
        kprintf("[elf] bias main=0x%llx interp=0x%llx (mlo=0x%llx ilo=0x%llx)\n",
                (unsigned long long)m_bias, (unsigned long long)i_bias,
                (unsigned long long)mlo, (unsigned long long)ilo);
    }

    u64 mbase, mend, mphdr;
    if (load_loads(main_img, main_size, eh, m_bias, &mbase, &mend, &mphdr) != 0) {
        kfree(interp_img);
        return -1;
    }

    u64 ibase, iend, iphdr;
    if (load_loads(interp_img, interp_sz, ieh, i_bias, &ibase, &iend, &iphdr) != 0) {
        kfree(interp_img);
        return -1;
    }
    kfree(interp_img);

    memset(out, 0, sizeof(*out));
    out->is_dyn = 1;
    out->entry = ieh->e_entry + i_bias; /* jump to interp (biased) */
    out->main_entry = eh->e_entry + m_bias;
    out->interp_base = ibase;
    out->load_base = mbase;
    out->load_end = mend > iend ? mend : iend;
    out->phnum = eh->e_phnum;
    out->phentsize = eh->e_phentsize;
    out->phdr_addr = mphdr ? mphdr : mbase + eh->e_phoff;

    kprintf("[elf] dyn main entry=0x%llx phdr=0x%llx interp_base=0x%llx interp_entry=0x%llx\n",
            (unsigned long long)out->main_entry,
            (unsigned long long)out->phdr_addr,
            (unsigned long long)out->interp_base,
            (unsigned long long)out->entry);
    return 0;
}
