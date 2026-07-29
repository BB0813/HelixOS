#pragma once

#include "helix/types.h"

/* Minimal ELF64 loader — static and dynamic (PT_INTERP). */
struct elf_load_info {
    u64 entry;       /* entry to jump to (interp entry if dynamic) */
    u64 load_base;   /* min loaded vaddr (main, after bias) */
    u64 load_end;
    u64 phdr_addr;   /* vaddr of main program headers in user space */
    u16 phnum;
    u16 phentsize;
    u64 interp_base; /* 0 if static; load bias of interpreter */
    u64 main_entry;  /* original e_entry of main (aux AT_ENTRY) */
    int is_dyn;
};

/* Load static image (no INTERP). */
int elf_load_image(const void *image, u64 size, struct elf_load_info *out);

/* Load dynamic: main + interpreter from VFS path in PT_INTERP.
 * Loads main at preferred/PIE base, interp at separate base; out->entry = interp entry. */
int elf_load_dynamic(const void *main_img, u64 main_size, struct elf_load_info *out);
