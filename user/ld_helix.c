/*
 * ld-helix — minimal freestanding "dynamic linker" for HelixOS M6 demos.
 *
 * Not musl ld.so. Protocol:
 *   Kernel loads main ELF (ET_DYN) and this interp, sets auxv:
 *     AT_BASE  = this interp's load base
 *     AT_ENTRY = main's entry
 *     AT_PHDR/PHENT/PHNUM = main's phdrs
 *   We jump to AT_ENTRY with the same stack (argc/argv/env/aux).
 *
 * Linked as ET_DYN-like fixed high base 0x50000000 (kernel applies bias 0 for us
 * by loading at p_vaddr).
 */
#include "usys.h"

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_BASE   7
#define AT_ENTRY  9

typedef unsigned long ulong;

void ld_helix_main(ulong *sp);

static ulong aux_get(ulong *aux, ulong key)
{
    for (int i = 0; aux[i] != AT_NULL; i += 2) {
        if (aux[i] == key)
            return aux[i + 1];
    }
    return 0;
}

/* Entry: stack is Linux-style; find auxv and jump to AT_ENTRY */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov %%rsp, %%rdi\n\t"
        "call ld_helix_main\n\t"
        "mov $60, %%eax\n\t"
        "xor %%edi, %%edi\n\t"
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        :
        : "memory");
}

void ld_helix_main(ulong *sp)
{
    ulong argc = sp[0];
    ulong *argv = sp + 1;
    ulong *p = argv + argc + 1; /* skip env */
    while (*p)
        p++;
    p++; /* end of env */
    ulong *aux = p;

    ulong entry = aux_get(aux, AT_ENTRY);
    if (!entry) {
        /* write error and exit */
        const char *msg = "ld-helix: no AT_ENTRY\n";
        long n = 0;
        while (msg[n])
            n++;
        __asm__ volatile("syscall"
                         :
                         : "a"(1), "D"(2), "S"(msg), "d"(n)
                         : "rcx", "r11", "memory");
        __asm__ volatile("syscall" : : "a"(60), "D"(1) : "rcx", "r11", "memory");
        for (;;)
            ;
    }

    /* Jump to main entry with original user stack in rsp.
     * SysV: argc already on stack; clear frame. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        "jmp *%1\n\t"
        :
        : "r"(sp), "r"(entry)
        : "memory");
}
