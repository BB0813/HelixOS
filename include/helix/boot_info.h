#pragma once

#include "helix/types.h"

/* Handoff from UEFI boot path to early kernel (single image for M1). */
struct helix_mmap_entry {
    u32 type;       /* EFI_MEMORY_TYPE */
    u32 pad;
    u64 phys_start;
    u64 virt_start;
    u64 npages;     /* 4K pages */
    u64 attrs;
};

struct helix_boot_info {
    struct helix_mmap_entry *mmap;
    u64 mmap_count;
    u64 phys_ceiling;   /* exclusive max physical address observed */
    u64 image_base;     /* best-effort; 0 if unknown */
    u64 image_size;
    u64 kernel_stack_top; /* if non-zero, early main may assume RSP already set */

    /* M9 framebuffer (GOP) — 0 if unavailable */
    u64 fb_addr;        /* physical base address */
    u64 fb_size;        /* total bytes */
    u32 fb_width;       /* horizontal resolution */
    u32 fb_height;      /* vertical resolution */
    u32 fb_pitch;       /* bytes per scanline */
    u32 fb_bpp;         /* bits per pixel (32 expected) */
};

void kernel_early_main(struct helix_boot_info *info);
