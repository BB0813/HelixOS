/* msh — HelixOS userspace shell (M11).
 *
 * A real shell built on fork/exec/waitpid/pipe/dup2:
 *   - reads lines from fd 0
 *   - splits on '|' into pipeline stages
 *   - each stage: tokenize → builtin or fork+execve
 *   - parent waits for children, prints exit status
 * Builtins: echo, cd, ls, cat, exit, help.
 */
#include "usys.h"

#define SYS_dup2       33
#define SYS_fork       57
#define SYS_execve     59
#define SYS_exit       60
#define SYS_wait4      61
#define SYS_pipe       22
#define SYS_getcwd     79
#define SYS_getpid     39
#define SYS_open        2
#define SYS_close       3
#define SYS_read        0
#define SYS_write       1
#define SYS_getdents64 217
#define SYS_mkdir       83
#define SYS_yield       24
#define SYS_uname       63
#define SYS_exit_group 231

#define WNOHANG 1
#define MSH_LINE_MAX 256
#define MSH_ARGV_MAX 16

static long usys6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static void msh_write(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    usys6(SYS_write, 1, (long)s, n, 0, 0, 0);
}

static int msh_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Read a line from fd 0 (blocking loop on EAGAIN). Returns 0 on EOF. */
static long msh_readline(char *buf, long cap)
{
    long i = 0;
    for (;;) {
        char c = 0;
        long r = usys6(SYS_read, 0, (long)&c, 1, 0, 0, 0);
        if (r == 0)
            return i > 0 ? i : 0; /* EOF */
        if (r < 0) {
            usys6(SYS_yield, 0, 0, 0, 0, 0, 0);
            continue;
        }
        if (c == '\n' || c == '\r')
            break;
        if (c == 8 || c == 127) { /* backspace */
            if (i > 0) {
                i--;
                msh_write("\b \b");
            }
            continue;
        }
        if (i + 1 < cap) {
            buf[i++] = c;
            usys6(SYS_write, 1, (long)&c, 1, 0, 0, 0); /* echo */
        }
    }
    buf[i] = 0;
    return i;
}

/* Split a stage into argv tokens. Returns argc. */
static int msh_tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
        if (argc >= MSH_ARGV_MAX - 1)
            break;
    }
    argv[argc] = 0;
    return argc;
}

/* --- builtins --- */
static int bi_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            msh_write(" ");
        msh_write(argv[i]);
    }
    msh_write("\n");
    return 0;
}

static int bi_cat(int argc, char **argv)
{
    if (argc < 2) {
        msh_write("cat: need file\n");
        return 1;
    }
    long fd = usys6(SYS_open, (long)argv[1], 0, 0, 0, 0, 0);
    if (fd < 0) {
        msh_write("cat: open failed\n");
        return 1;
    }
    char buf[128];
    for (;;) {
        long n = usys6(SYS_read, fd, (long)buf, 128, 0, 0, 0);
        if (n > 0) {
            usys6(SYS_write, 1, (long)buf, n, 0, 0, 0);
        } else if (n == 0) {
            break;
        } else {
            usys6(SYS_yield, 0, 0, 0, 0, 0, 0);
        }
    }
    usys6(SYS_close, fd, 0, 0, 0, 0, 0);
    return 0;
}

static int bi_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    long fd = usys6(SYS_open, (long)path, 0, 0, 0, 0, 0);
    if (fd < 0) {
        msh_write("ls: open failed\n");
        return 1;
    }
    char buf[512];
    long n = usys6(SYS_getdents64, fd, (long)buf, 512, 0, 0, 0);
    if (n < 0) {
        msh_write("ls: getdents failed\n");
        usys6(SYS_close, fd, 0, 0, 0, 0, 0);
        return 1;
    }
    long off = 0;
    while (off < n) {
        struct linux_dirent64 { unsigned long long d_ino; long long d_off;
            unsigned short d_reclen; unsigned char d_type; char d_name[]; };
        struct linux_dirent64 *de = (void *)(buf + off);
        msh_write(de->d_name);
        msh_write("\n");
        if (de->d_reclen == 0)
            break;
        off += de->d_reclen;
    }
    usys6(SYS_close, fd, 0, 0, 0, 0, 0);
    return 0;
}

static int bi_help(void)
{
    msh_write("msh: HelixOS shell (M11)\n");
    msh_write("  echo | cat | ls | cd | exit | help\n");
    msh_write("  pipelines: cmd1 | cmd2 | ...\n");
    return 0;
}

/* echo builtin that walks a NULL-terminated argv directly. */
static void bi_echo2(char **argv)
{
    for (int i = 1; argv[i]; i++) {
        if (i > 1)
            msh_write(" ");
        msh_write(argv[i]);
    }
    msh_write("\n");
}

/* Execute one pipeline stage in the child; returns 0 on exec-success path. */
static void run_stage(char **argv, int infd, int outfd)
{
    /* Hook up pipe ends onto stdio */
    if (infd != 0) {
        usys6(SYS_dup2, infd, 0, 0, 0, 0, 0);
        usys6(SYS_close, infd, 0, 0, 0, 0, 0);
    }
    if (outfd != 1) {
        usys6(SYS_dup2, outfd, 1, 0, 0, 0, 0);
        usys6(SYS_close, outfd, 0, 0, 0, 0, 0);
    }
    /* Builtins run inline in the forked child, then exit. */
    if (msh_streq(argv[0], "echo")) {
        bi_echo2(argv);
        usys6(SYS_exit, 0, 0, 0, 0, 0, 0);
        for (;;)
            ;
    }
    if (msh_streq(argv[0], "cat")) {
        /* read stdin → stdout until EOF; loop on EAGAIN with yield */
        char buf[128];
        for (;;) {
            long n = usys6(SYS_read, 0, (long)buf, 128, 0, 0, 0);
            if (n > 0) {
                usys6(SYS_write, 1, (long)buf, n, 0, 0, 0);
            } else if (n == 0) {
                break; /* EOF */
            } else {
                usys6(SYS_yield, 0, 0, 0, 0, 0, 0);
            }
        }
        usys6(SYS_exit, 0, 0, 0, 0, 0, 0);
        for (;;)
            ;
    }
    long r = usys6(SYS_execve, (long)argv[0], (long)argv, 0, 0, 0, 0);
    msh_write("msh: exec failed\n");
    (void)r;
    usys6(SYS_exit, 127, 0, 0, 0, 0, 0);
    for (;;)
        ;
}

static int run_pipeline(char **cmds, int ncmds, char **argv_list[])
{
    (void)cmds;
    int prev_fd = 0;
    for (int i = 0; i < ncmds; i++) {
        int fds[2];
        int next_fd = 1;
        if (i < ncmds - 1) {
            if (usys6(SYS_pipe, (long)fds, 0, 0, 0, 0, 0) != 0)
                return 1;
            next_fd = fds[1];
        }
        long pid = usys6(SYS_fork, 0, 0, 0, 0, 0, 0);
        if (pid == 0) {
            if (i < ncmds - 1)
                usys6(SYS_close, fds[0], 0, 0, 0, 0, 0);
            run_stage(argv_list[i], prev_fd, next_fd);
        }
        /* parent: close the fds we passed */
        if (prev_fd != 0)
            usys6(SYS_close, prev_fd, 0, 0, 0, 0, 0);
        if (i < ncmds - 1)
            usys6(SYS_close, fds[1], 0, 0, 0, 0, 0);
        prev_fd = i < ncmds - 1 ? fds[0] : 0;
    }
    /* wait for all children (poll with yield — wait4 is non-blocking) */
    int last_status = 0;
    int reaped = 0;
    while (reaped < ncmds) {
        int status = 0;
        long wpid = usys6(SYS_wait4, -1, (long)&status, 0, 0, 0, 0);
        if (wpid > 0) {
            last_status = status;
            reaped++;
        } else if (wpid == -10) { /* ECHILD */
            break;
        } else {
            usys6(SYS_yield, 0, 0, 0, 0, 0, 0);
        }
    }
    return (last_status >> 8) & 0xFF;
}

static void msh_prompt(void)
{
    msh_write("msh> ");
}

/* Execute one command line (may contain '|'). Returns exit status. */
static int msh_exec_line(const char *cline)
{
    char line[MSH_LINE_MAX];
    long i = 0;
    while (cline[i] && i < MSH_LINE_MAX - 1) {
        line[i] = cline[i];
        i++;
    }
    line[i] = 0;

    /* split into pipeline stages on '|' */
    char *stages[8];
    int nstages = 0;
    {
        char *p = line;
        stages[nstages++] = p;
        while (*p && nstages < 8) {
            if (*p == '|') {
                *p = 0;
                stages[nstages++] = p + 1;
            }
            p++;
        }
    }
    char **argv_list[8];
    char **argv0 = argv_list[0] = (char *[MSH_ARGV_MAX]){0};
    int argc0 = msh_tokenize(stages[0], argv0);
    if (argc0 == 0)
        return 0;
    if (msh_streq(argv0[0], "exit"))
        return -1; /* signal quit */
    if (msh_streq(argv0[0], "help")) {
        bi_help();
        return 0;
    }
    if (nstages == 1) {
        if (msh_streq(argv0[0], "echo")) return bi_echo(argc0, argv0);
        if (msh_streq(argv0[0], "cat"))  return bi_cat(argc0, argv0);
        if (msh_streq(argv0[0], "ls"))   return bi_ls(argc0, argv0);
        if (msh_streq(argv0[0], "cd")) {
            msh_write("cd: no cwd support (stays /)\n");
            return 0;
        }
    }
    for (int i = 1; i < nstages; i++) {
        char **av = argv_list[i] = (char *[MSH_ARGV_MAX]){0};
        msh_tokenize(stages[i], av);
    }
    return run_pipeline(stages, nstages, argv_list);
}

void msh_main(int argc, char **argv)
{
    msh_write("HelixOS msh (M11) — fork/exec/waitpid/pipe shell\n");

    /* Non-interactive: msh -c "command" */
    if (argc >= 3 && msh_streq(argv[1], "-c")) {
        int st = msh_exec_line(argv[2]);
        usys6(SYS_exit_group, st == -1 ? 0 : st, 0, 0, 0, 0, 0);
        for (;;)
            ;
    }

    char line[MSH_LINE_MAX];
    for (;;) {
        msh_prompt();
        long n = msh_readline(line, MSH_LINE_MAX);
        if (n == 0) {
            msh_write("\n");
            break; /* EOF on stdin */
        }
        int st = msh_exec_line(line);
        if (st == -1)
            break;
        if (st != 0) {
            msh_write("msh: status ");
            char tmp[8];
            int ti = 0;
            int v = st;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti) msh_write(&(char){tmp[--ti]});
            msh_write("\n");
        }
    }
    usys6(SYS_exit_group, 0, 0, 0, 0, 0, 0);
    for (;;)
        ;
}

/* Linux process entry: rsp -> argc, argv pointers, ... */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov (%%rsp), %%rdi\n\t"       /* argc */
        "lea 8(%%rsp), %%rsi\n\t"      /* argv */
        "call msh_main\n\t"
        "mov %%rax, %%rdi\n\t"
        "mov $231, %%eax\n\t"          /* exit_group */
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        :
        : "memory");
}
