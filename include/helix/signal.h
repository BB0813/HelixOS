#pragma once

#include "helix/types.h"

/* Linux signal numbers (subset). */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

#define SIG_DFL ((u64)0)
#define SIG_IGN ((u64)1)

/* How many handlers we store (1..NSIG-1). */
#define HELIX_NSIG 32

struct task;

/* sa_handler is SIG_DFL / SIG_IGN / user VA (user handlers not run in M13). */
struct helix_sigaction {
    u64 sa_handler;
    u64 sa_flags;
    u64 sa_restorer;
    u64 sa_mask; /* simple one-word mask */
};

void signal_task_init(struct task *t);
void signal_send(struct task *t, int sig);
/* Deliver pending unblocked signals for current task.
 * May call task_exit_current (does not return) if default action is terminate. */
void signal_deliver_current(void);
/* After exit: notify parent with SIGCHLD. */
void signal_on_exit(struct task *t);
