#pragma once

#include "helix/types.h"

#define TASK_MAX        8
#define TASK_NAME_MAX   16
#define TASK_MAX_PAGES  2048  /* max tracked user pages per task (8 MiB) */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_ZOMBIE,
};

/* Saved user context for cooperative switch (sysret path registers). */
struct task_regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 rip;     /* user RIP (also RCX on sysret path snapshot) */
    u64 rflags;  /* user RFLAGS (also R11) */
    u64 rsp;     /* user RSP */
};

struct vfs_file; /* opaque */

struct task {
    int              pid;
    struct task     *parent;
    enum task_state  state;
    int              exit_code;
    char             name[TASK_NAME_MAX];
    struct task_regs regs;
    u64              user_stack_top;
    u64              kernel_stack;
    u64              kernel_stack_top;
    struct vfs_file *fds[16];
    u64              brk_start;   /* end of data segment */
    u64              brk_curr;    /* current program break */
    u64              user_pages[TASK_MAX_PAGES]; /* tracked user page phys addrs */
    int              user_page_count;
};

void          task_init(void);
struct task  *task_current(void);
struct task  *task_create(const char *name, u64 entry, u64 user_sp);
void          task_exit_current(int code);
void          task_yield(void);
/* Enter first ready task (does not return to caller if tasks run forever). */
void          task_start_user(void);
/* If set, task_exit with no READY tasks jumps here instead of idle loop. */
void          task_set_exit_all_hook(void (*hook)(void));
int           task_count_alive(void);
void          task_dump(void);
/* Track a user page allocation for fork/exec cleanup. */
void          task_track_user_page(struct task *t, u64 vaddr, u64 phys);
struct task  *task_fork(struct task *parent);
/* Free all tracked user pages (called on exec/exit). */
void          task_free_user_pages(struct task *t);
/* Look up a child by pid (0 = any child); NULL if none. */
struct task  *task_find_child(struct task *parent, int pid);
/* 1 if a child has exited; -1 if no such child. */
int           task_child_exited(struct task *parent, int pid);
/* Free a zombie's resources and mark its slot unused. */
void          task_reap(struct task *t);
/* pid of current task (0 if none). */
int           task_getpid(void);
/* M11 wait4: wait for a child. want=0 any, >0 specific, <-1 group.
 * Returns reaped pid; 0 if WNOHANG and none ready; -ECHILD if no children. */
int           task_wait(int want, int *out_status, int options);

/* Called from syscall path when current task must resume another. */
void          sched_switch_to(struct task *next);
