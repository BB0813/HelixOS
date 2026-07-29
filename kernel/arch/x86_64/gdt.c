#include "helix/gdt.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/types.h"

/* Flat long-mode GDT: null | kcode | kdata | udata | ucode | tss(2 slots) */
struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed));

struct tss_entry {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct gdt_entry g_gdt[8];
static struct tss_entry g_tss;
static struct gdt_ptr   g_gdtr;

static void gdt_set(int idx, u32 base, u32 limit, u8 access, u8 gran)
{
    g_gdt[idx].base_low    = (u16)(base & 0xFFFF);
    g_gdt[idx].base_mid    = (u8)((base >> 16) & 0xFF);
    g_gdt[idx].base_high   = (u8)((base >> 24) & 0xFF);
    g_gdt[idx].limit_low   = (u16)(limit & 0xFFFF);
    g_gdt[idx].granularity = (u8)((limit >> 16) & 0x0F);
    g_gdt[idx].granularity |= (u8)(gran & 0xF0);
    g_gdt[idx].access      = access;
}

static void gdt_set_tss(int idx, u64 base, u32 limit)
{
    /* 16-byte TSS descriptor spanning two GDT slots */
    g_gdt[idx].limit_low   = (u16)(limit & 0xFFFF);
    g_gdt[idx].base_low    = (u16)(base & 0xFFFF);
    g_gdt[idx].base_mid    = (u8)((base >> 16) & 0xFF);
    g_gdt[idx].access      = 0x89; /* present, type=9 (available 64-bit TSS) */
    g_gdt[idx].granularity = (u8)((limit >> 16) & 0x0F);
    g_gdt[idx].base_high   = (u8)((base >> 24) & 0xFF);
    /* high half in next entry */
    g_gdt[idx + 1].limit_low   = (u16)((base >> 32) & 0xFFFF);
    g_gdt[idx + 1].base_low    = (u16)((base >> 48) & 0xFFFF);
    g_gdt[idx + 1].base_mid    = 0;
    g_gdt[idx + 1].access      = 0;
    g_gdt[idx + 1].granularity = 0;
    g_gdt[idx + 1].base_high   = 0;
}

void gdt_set_tss_rsp0(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

u16 gdt_kernel_cs(void)
{
    return GDT_KERNEL_CS;
}

void gdt_init(void)
{
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iomap_base = sizeof(g_tss);

    /* 0: null */
    gdt_set(0, 0, 0, 0, 0);
    /* 1: kernel code 0x08 — L=1, P, S, exec/read, DPL0 */
    gdt_set(1, 0, 0, 0x9A, 0x20);
    /* 2: kernel data 0x10 */
    gdt_set(2, 0, 0, 0x92, 0x00);
    /* 3: user data 0x18 — DPL3 */
    gdt_set(3, 0, 0, 0xF2, 0x00);
    /* 4: user code 0x20 — L=1, DPL3 */
    gdt_set(4, 0, 0, 0xFA, 0x20);
    /* 5-6: TSS */
    gdt_set_tss(5, (u64)(uintptr_t)&g_tss, sizeof(g_tss) - 1);

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (u64)(uintptr_t)g_gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "r"(&g_gdtr)
        : "rax", "memory");

    kprintf("[gdt] kernel CS=0x%x DS=0x%x user CS=0x%x DS=0x%x TSS=0x%x\n",
            GDT_KERNEL_CS, GDT_KERNEL_DS, GDT_USER_CS, GDT_USER_DS, GDT_TSS);
}
