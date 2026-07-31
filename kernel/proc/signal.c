#include "helix/signal.h"
#include "helix/task.h"
#include "helix/kprintf.h"

/* Default: 0=ignore, 1=terminate. */
static int sig_default_terminate(int sig)
{
    switch (sig) {
    case SIGCHLD:
    case SIGCONT:
    case SIGSTOP:
    case SIGTSTP:
        return 0;
    default:
        return 1;
    }
}

void signal_task_init(struct task *t)
{
    if (!t)
        return;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->term_sig = 0;
    for (int i = 0; i < HELIX_NSIG; i++) {
        t->sighand[i].sa_handler = SIG_DFL;
        t->sighand[i].sa_flags = 0;
        t->sighand[i].sa_restorer = 0;
        t->sighand[i].sa_mask = 0;
    }
}

void signal_send(struct task *t, int sig)
{
    if (!t || sig < 1 || sig >= HELIX_NSIG)
        return;
    if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE)
        return;

    t->sig_pending |= (1ull << sig);
    kprintf("[sig] send sig=%d -> pid=%d pending=0x%llx\n",
            sig, t->pid, (unsigned long long)t->sig_pending);
}

void signal_on_exit(struct task *t)
{
    if (!t || !t->parent)
        return;
    if (t->parent->state == TASK_UNUSED || t->parent->state == TASK_ZOMBIE)
        return;
    signal_send(t->parent, SIGCHLD);
}

/* Returns 1 if task must die. */
static int signal_apply_one(struct task *t, int sig)
{
    u64 handler = SIG_DFL;
    if (sig > 0 && sig < HELIX_NSIG)
        handler = t->sighand[sig].sa_handler;

    if (sig == SIGKILL)
        return 1;

    if (handler == SIG_IGN)
        return 0;

    if (handler == SIG_DFL) {
        if (sig == SIGCHLD || sig == SIGCONT)
            return 0;
        return sig_default_terminate(sig) ? 1 : 0;
    }

    /* Custom user handler: M13 has no signal frame yet — drop & keep alive. */
    kprintf("[sig] pid=%d sig=%d user handler 0x%llx — drop (no frame)\n",
            t->pid, sig, (unsigned long long)handler);
    return 0;
}

void signal_deliver_current(void)
{
    struct task *t = task_current();
    if (!t || t->state != TASK_RUNNING)
        return;

    u64 pend = t->sig_pending;
    if (!pend)
        return;

    for (int sig = 1; sig < HELIX_NSIG; sig++) {
        u64 bit = 1ull << sig;
        if (!(pend & bit))
            continue;
        if (sig != SIGKILL && (t->sig_blocked & bit))
            continue;

        t->sig_pending &= ~bit;

        if (signal_apply_one(t, sig)) {
            kprintf("[sig] pid=%d terminated by signal %d\n", t->pid, sig);
            t->term_sig = sig;
            task_exit_current(sig);
            return;
        }
    }
}
