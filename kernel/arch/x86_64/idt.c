#include "helix/idt.h"
#include "helix/irq.h"
#include "helix/gdt.h"
#include "helix/kprintf.h"
#include "helix/panic.h"
#include "helix/types.h"
#include "helix/string.h"
#include "helix/pic.h"

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_idtr;
static u16 g_kernel_cs;

extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);
extern void isr_stub_32(void);
extern void isr_stub_33(void);
extern void isr_stub_34(void);
extern void isr_stub_35(void);
extern void isr_stub_36(void);
extern void isr_stub_37(void);
extern void isr_stub_38(void);
extern void isr_stub_39(void);
extern void isr_stub_40(void);
extern void isr_stub_41(void);
extern void isr_stub_42(void);
extern void isr_stub_43(void);
extern void isr_stub_44(void);
extern void isr_stub_45(void);
extern void isr_stub_46(void);
extern void isr_stub_47(void);

static void (*const isr_stubs[48])(void) = {
    isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
    isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
    isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
    isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
    isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
    isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
    isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
    isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
    isr_stub_32, isr_stub_33, isr_stub_34, isr_stub_35,
    isr_stub_36, isr_stub_37, isr_stub_38, isr_stub_39,
    isr_stub_40, isr_stub_41, isr_stub_42, isr_stub_43,
    isr_stub_44, isr_stub_45, isr_stub_46, isr_stub_47,
};

static const char *const exc_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "RES",
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "22", "23",
    "24", "25", "26", "27", "#HV", "#VC", "#SX", "31",
};

struct interrupt_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vector;
    u64 errcode;
    u64 rip, cs, rflags, rsp, ss;
};

void isr_dispatch(struct interrupt_frame *f)
{
    u64 vec = f->vector;

    /* Hardware IRQs (PIC remapped to 32..47) */
    if (vec >= PIC_IRQ_BASE && vec < PIC_IRQ_BASE + PIC_IRQ_COUNT) {
        irq_handle((u8)(vec - PIC_IRQ_BASE));
        return;
    }

    if (vec >= 32) {
        /* Spurious / unexpected vector — log once-style via kprintf then EOI-less return.
         * Do not panic: keeps system alive for shell debugging. */
        kprintf("[idt] unhandled vector %llu rip=0x%llx\n",
                (unsigned long long)vec,
                (unsigned long long)f->rip);
        return;
    }

    const char *name = exc_names[vec];
    if (vec == 14) {
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        panic("exception %s (%llu) err=0x%llx rip=0x%llx cr2=0x%llx",
              name, (unsigned long long)vec,
              (unsigned long long)f->errcode,
              (unsigned long long)f->rip,
              (unsigned long long)cr2);
    }
    panic("exception %s (%llu) err=0x%llx rip=0x%llx rsp=0x%llx",
          name, (unsigned long long)vec,
          (unsigned long long)f->errcode,
          (unsigned long long)f->rip,
          (unsigned long long)f->rsp);
}

void idt_set_gate(int vec, void (*handler)(void))
{
    if (vec < 0 || vec >= 256 || !handler)
        return;
    u64 addr = (u64)(uintptr_t)handler;
    struct idt_entry *e = &g_idt[vec];
    e->offset_low  = (u16)(addr & 0xFFFF);
    e->selector    = g_kernel_cs ? g_kernel_cs : GDT_KERNEL_CS;
    /* Use IST slot 0 for hardware IRQs (vectors 32..47) so the ISR runs
     * on a dedicated stack instead of the current task's kstack.
     * Without this, a timer IRQ fired mid-syscall would clobber the
     * syscall frame (the timer fires during syscalls like poll that
     * take long enough for ~1 tick). */
    e->ist         = (vec >= 32 && vec < 48) ? 1 : 0;
    e->type_attr   = 0x8E; /* present, DPL0, interrupt gate */
    e->offset_mid  = (u16)((addr >> 16) & 0xFFFF);
    e->offset_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    e->zero        = 0;
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));

    __asm__ volatile("mov %%cs, %0" : "=r"(g_kernel_cs));

    for (int i = 0; i < 48; i++)
        idt_set_gate(i, isr_stubs[i]);

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (u64)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idtr) : "memory");

    kprintf("[idt] loaded gates 0..47, CS=0x%x\n", (unsigned)g_kernel_cs);
}
