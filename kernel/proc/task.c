#include "helix/task.h"
#include "helix/gdt.h"
#include "helix/syscall.h"
#include "helix/pmm.h"
#include "helix/vfs.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/panic.h"
#include "helix/cpuio.h"
#include "helix/vmm.h"
#include "helix/paging.h"

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
    t->parent = 0;
    t->state = TASK_READY;
    t->exit_code = 0;
    t->cwd[0] = '/';
    t->cwd[1] = 0;
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
    /* Free inherited FDs on exit (zombie's fds are not task_current's).
     * Console stdio files (0–2) are static & shared — never free them.
     * But a dup2'd pipe/file on fd 0–2 must be released normally. */
    for (int i = 0; i < 16; i++) {
        struct vfs_file *f = t->fds[i];
        if (!f)
            continue;
        if (f->is_console)
            continue; /* static, never free */
        t->fds[i] = 0;
        f->refcount--;
        if (f->refcount > 0)
            continue;
        if (f->is_socket) {
            extern void net_sock_free(void *s);
            net_sock_free(f->fs_priv);
            extern void kfree(void *ptr);
            kfree(f);
        } else {
            extern int vfs_close(struct vfs_file *f);
            vfs_close(f);
        }
    }

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

void task_track_user_page(struct task *t, u64 vaddr, u64 phys)
{
    if (!t || t->user_page_count >= TASK_MAX_PAGES)
        return;
    t->user_pages[t->user_page_count++] = phys;
    (void)vaddr; /* vaddr stored for future COW/debug */
}

void task_free_user_pages(struct task *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->user_page_count; i++) {
        if (t->user_pages[i])
            pmm_free_page(t->user_pages[i]);
    }
    t->user_page_count = 0;
}

struct task *task_fork(struct task *parent)
{
    if (!parent)
        return 0;
    struct task *child = alloc_slot();
    if (!child)
        return 0;

    /* Copy task struct (name, regs, etc.) */
    memcpy(child, parent, sizeof(*child));
    child->pid = g_next_pid++;
    child->parent = parent;
    child->state = TASK_READY;
    child->exit_code = 0;
    child->user_page_count = 0; /* will be populated by vmm_copy */
    memset(child->user_pages, 0, sizeof(child->user_pages));

    /* Kernel stack: allocate new, copy parent's kernel stack content */
    u64 kphys = pmm_alloc_page();
    if (!kphys) {
        child->state = TASK_UNUSED;
        return 0;
    }
    memcpy((void *)(uintptr_t)kphys,
           (const void *)(uintptr_t)parent->kernel_stack, PAGE_SIZE);
    child->kernel_stack = kphys;
    child->kernel_stack_top = kphys + PAGE_SIZE;

    /* Duplicate file descriptors: share pointers, bump refcount so child
     * and parent each own their slot. */
    for (int i = 0; i < 16; i++) {
        if (child->fds[i]) {
            extern void fd_hold(struct vfs_file *f);
            fd_hold(child->fds[i]);
        }
    }

    /* Duplicate user page tables: child gets own PML4 with copied user pages */
    u64 parent_pml4 = paging_cr3();
    u64 child_pml4 = vmm_copy_user_page_tables(parent_pml4, child);
    if (!child_pml4) {
        pmm_free_page(kphys);
        child->state = TASK_UNUSED;
        return 0;
    }

    /* Load child's CR3, flush TLB, then restore parent's CR3 */
    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "mov %1, %%cr3\n\t"
        : : "r"(child_pml4), "r"(parent_pml4) : "memory"
    );

    kprintf("[task] fork pid=%d -> child pid=%d (rip=0x%llx)\n",
            parent->pid, child->pid,
            (unsigned long long)child->regs.rip);
    return child;
}

struct task *task_find_child(struct task *parent, int pid)
{
    if (!parent)
        return 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *c = &g_tasks[i];
        if (c->state == TASK_UNUSED || c->parent != parent)
            continue;
        if (pid == 0 || c->pid == pid)
            return c;
    }
    return 0;
}

int task_child_exited(struct task *parent, int pid)
{
    struct task *c = task_find_child(parent, pid);
    if (!c)
        return -1; /* no such child */
    return c->state == TASK_ZOMBIE ? 1 : 0;
}

void task_reap(struct task *t)
{
    if (!t)
        return;
    /* User pages + kernel stack already freed on exit.
     * FDs were closed in task_exit_current. */
    task_free_user_pages(t);
    if (t->kernel_stack)
        pmm_free_page(t->kernel_stack);
    t->kernel_stack = 0;
    /* Close any remaining FDs (should already be done in exit path). */
    for (int i = 0; i < 16; i++) {
        struct vfs_file *f = t->fds[i];
        if (!f || f->is_console)
            continue;
        t->fds[i] = 0;
        f->refcount--;
        if (f->refcount > 0)
            continue;
        if (f->is_socket) {
            extern void net_sock_free(void *s);
            net_sock_free(f->fs_priv);
            extern void kfree(void *ptr);
            kfree(f);
        } else {
            extern int vfs_close(struct vfs_file *f);
            vfs_close(f);
        }
    }
    t->state = TASK_UNUSED;
    kprintf("[task] reap pid=%d\n", t->pid);
}

int task_getpid(void)
{
    struct task *t = task_current();
    return t ? t->pid : 0;
}

int task_wait(int want, int *out_status, int options)
{
    (void)options; /* WNOHANG handled by caller polling */
    struct task *parent = task_current();
    if (!parent)
        return -1;

    if (want < -1)
        want = -want;
    else if (want < 0)
        want = 0; /* -1 = any child */

    int alive = 0;
    struct task *victim = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *c = &g_tasks[i];
        if (c->state == TASK_UNUSED || c->parent != parent)
            continue;
        if (want > 0 && c->pid != want)
            continue;
        if (c->state == TASK_ZOMBIE) {
            victim = c;
            break;
        }
        alive = 1;
    }
    if (victim) {
        int code = victim->exit_code;
        int pid = victim->pid;
        if (out_status)
            *out_status = (code & 0xFF) << 8;
        task_reap(victim);
        return pid;
    }
    if (!alive)
        return -1; /* ECHILD */
    /* Child alive but not a zombie yet. The scheduler only switches tasks
     * at the syscall return path, so we must NOT block here — return 0 and
     * let the caller (msh) poll with yield(). */
    return 0;
}
