#include "helix/task.h"
#include "helix/gdt.h"
#include "helix/syscall.h"
#include "helix/pmm.h"
#include "helix/vfs.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/panic.h"
#include "helix/cpuio.h"

extern u64 g_syscall_kstack;
extern void user_enter_asm(u64 entry, u64 user_rsp);

static struct task g_tasks[TASK_MAX];
static struct task *g_current;
static int g_next_pid = 1;
static void (*g_exit_all_hook)(void);

void task_set_exit_all_hook(void (*hook)(void))
{
    g_exit_all_hook = hook;
}

void task_init(void)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    g_current = 0;
    g_next_pid = 1;
}

struct task *task_current(void)
{
    return g_current;
}

int task_count_alive(void)
{
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_READY || g_tasks[i].state == TASK_RUNNING)
            n++;
    }
    return n;
}

void task_dump(void)
{
    kprintf("tasks:\n");
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED)
            continue;
        kprintf("  pid=%d state=%d name=%s rip=0x%llx\n",
                g_tasks[i].pid, (int)g_tasks[i].state, g_tasks[i].name,
                (unsigned long long)g_tasks[i].regs.rip);
    }
}

static struct task *alloc_slot(void)
{
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED)
            return &g_tasks[i];
    }
    return 0;
}

struct task *task_create(const char *name, u64 entry, u64 user_sp)
{
    struct task *t = alloc_slot();
    if (!t)
        return 0;

    memset(t, 0, sizeof(*t));
    t->pid = g_next_pid++;
    t->state = TASK_READY;
    t->exit_code = 0;
    if (name) {
        size_t n = strlen(name);
        if (n >= TASK_NAME_MAX)
            n = TASK_NAME_MAX - 1;
        memcpy(t->name, name, n);
        t->name[n] = 0;
    }

    /* kernel stack: one page */
    u64 kphys = pmm_alloc_page();
    if (!kphys) {
        t->state = TASK_UNUSED;
        return 0;
    }
    t->kernel_stack = kphys;
    t->kernel_stack_top = kphys + PAGE_SIZE;

    t->user_stack_top = user_sp;
    t->regs.rip = entry;
    t->regs.rsp = user_sp;
    t->regs.rflags = 0x200; /* IF */
    t->regs.rax = 0;
    /* stdio filled on first enter via fd_init_task_stdio when current */

    kprintf("[task] create pid=%d name=%s entry=0x%llx usp=0x%llx kstack=0x%llx\n",
            t->pid, t->name,
            (unsigned long long)entry,
            (unsigned long long)user_sp,
            (unsigned long long)t->kernel_stack_top);
    return t;
}

static struct task *pick_next(struct task *except)
{
    /* Round-robin from after `except` */
    int start = 0;
    if (except) {
        start = (int)(except - g_tasks) + 1;
    }
    for (int k = 0; k < TASK_MAX; k++) {
        int i = (start + k) % TASK_MAX;
        if (g_tasks[i].state == TASK_READY)
            return &g_tasks[i];
    }
    return 0;
}

void task_exit_current(int code)
{
    struct task *t = g_current;
    if (!t)
        return;
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    kprintf("[task] zombie pid=%d code=%d\n", t->pid, code);

    struct task *n = pick_next(t);
    if (!n) {
        kprintf("[task] no runnable tasks left\n");
        g_current = 0;
        if (g_exit_all_hook) {
            void (*h)(void) = g_exit_all_hook;
            g_exit_all_hook = 0;
            /* Jump to kernel idle stack path: just call idle */
            h();
        }
        extern void kernel_idle_loop(void);
        kernel_idle_loop();
    }
    n->state = TASK_RUNNING;
    g_current = n;
    g_syscall_kstack = n->kernel_stack_top;
    gdt_set_tss_rsp0(n->kernel_stack_top);
}

void task_yield(void)
{
    struct task *t = g_current;
    if (!t)
        return;
    struct task *n = pick_next(t);
    if (!n || n == t)
        return; /* alone */

    t->state = TASK_READY;
    /* user rip/rsp/rflags already snapshotted in syscall_entry_c before dispatch */
    n->state = TASK_RUNNING;
    g_current = n;
    g_syscall_kstack = n->kernel_stack_top;
    gdt_set_tss_rsp0(n->kernel_stack_top);
    kprintf("[sched] yield %d -> %d\n", t->pid, n->pid);
}

void task_start_user(void)
{
    struct task *t = pick_next(0);
    if (!t)
        panic("task_start_user: no task");
    t->state = TASK_RUNNING;
    g_current = t;
    g_syscall_kstack = t->kernel_stack_top;
    gdt_set_tss_rsp0(t->kernel_stack_top);
    fd_init_task_stdio();

    kprintf("[task] enter user pid=%d entry=0x%llx\n",
            t->pid, (unsigned long long)t->regs.rip);
    user_enter_asm(t->regs.rip, t->regs.rsp);
    panic("user_enter_asm returned");
}

void sched_switch_to(struct task *next)
{
    if (!next)
        return;
    if (g_current && g_current->state == TASK_RUNNING)
        g_current->state = TASK_READY;
    next->state = TASK_RUNNING;
    g_current = next;
    g_syscall_kstack = next->kernel_stack_top;
    gdt_set_tss_rsp0(next->kernel_stack_top);
}
