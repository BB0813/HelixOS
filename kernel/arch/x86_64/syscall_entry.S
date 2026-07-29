/* SYSCALL entry + Ring3 enter — MS x64 freestanding Helix kernel
 *
 * SYSCALL: RCX=user RIP, R11=user RFLAGS, RAX=nr
 * args in RDI,RSI,RDX,R10,R8,R9 (Linux user convention)
 *
 * user_enter_asm(entry=RCX, user_rsp=RDX) — MS x64 args; must not clobber
 * them before the iretq frame is built (wrmsr uses EAX/EDX/ECX!).
 */
    .text
    .global syscall_entry_asm
    .global user_enter_asm
    .extern syscall_entry_c
    .extern g_syscall_kstack
    .extern g_syscall_user_rsp

syscall_entry_asm:
    mov %rsp, g_syscall_user_rsp(%rip)
    mov g_syscall_kstack(%rip), %rsp

    push g_syscall_user_rsp(%rip)   /* user_rsp */
    push %r11                       /* rflags */
    push %rcx                       /* rip */
    push %r9
    push %r8
    push %r10
    push %rdx
    push %rsi
    push %rdi
    push %rax                       /* nr */

    mov %rsp, %rcx                  /* MS arg0 */
    sub $32, %rsp
    call syscall_entry_c
    add $32, %rsp

    add $8, %rsp                    /* drop nr */
    pop %rdi
    pop %rsi
    pop %rdx
    pop %r10
    pop %r8
    pop %r9
    pop %rcx                        /* user rip */
    pop %r11                        /* user rflags */
    pop %rsp                        /* user rsp */
    sysretq

user_enter_asm:
    cli
    /* Preserve MS args: entry in RCX, user_rsp in RDX across WRMSR. */
    mov %rcx, %r8                   /* r8  = entry */
    mov %rdx, %r9                   /* r9  = user_rsp */

    xor %eax, %eax
    xor %edx, %edx
    mov $0xC0000100, %ecx           /* IA32_FS_BASE */
    wrmsr
    mov $0xC0000101, %ecx           /* IA32_GS_BASE */
    wrmsr

    xor %rax, %rax
    xor %rbx, %rbx
    xor %rbp, %rbp
    xor %r10, %r10
    xor %r11, %r11
    xor %r12, %r12
    xor %r13, %r13
    xor %r14, %r14
    xor %r15, %r15
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs

    pushq $0x1B                     /* SS */
    push %r9                        /* RSP = saved user_rsp */
    pushq $0x2                      /* RFLAGS bit1, IF=0 */
    pushq $0x23                     /* CS */
    push %r8                        /* RIP = saved entry */
    iretq
