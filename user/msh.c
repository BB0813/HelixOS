/* msh — HelixOS userspace shell (M12).
 *
 * A real shell built on fork/exec/waitpid/pipe/dup2/chdir:
 *   - reads lines from fd 0
 *   - splits on '|' into pipeline stages
 *   - each stage: tokenize → builtin or fork+execve
 *   - parent waits for children, prints exit status
 * Builtins: echo, cd, pwd, ls, cat, exit, help.
 */
#include "usys.h"

#define SYS_dup2       33
#define SYS_fork       57
#define SYS_execve     59
#define SYS_exit       60
#define SYS_wait4      61
#define SYS_pipe       22
#define SYS_getcwd     79
#define SYS_chdir      80
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

/* D3: line editor — cursor + history ring + Ctrl+A/E/W/U + arrow keys. */
#define MSH_HIST_MAX 16
static char msh_history[MSH_HIST_MAX][MSH_LINE_MAX];
static int  msh_hcount = 0;
static int  msh_hidx   = -1;       /* -1 = on draft line; 0..hcount-1 = history index */
static char msh_draft[MSH_LINE_MAX];

static void msh_redraw(const char *line, long cur, long len)
{
    /* Move to col 0, rewrite whole line, then back up (len - cur) chars. */
    usys6(SYS_write, 1, (long)"\r", 1, 0, 0, 0);
    usys6(SYS_write, 1, (long)line, len, 0, 0, 0);
    /* Erase stale tail by overwriting with spaces (cheap on serial log too). */
    usys6(SYS_write, 1, (long)" ", 1, 0, 0, 0);
    usys6(SYS_write, 1, (long)"\r", 1, 0, 0, 0);
    usys6(SYS_write, 1, (long)line, len, 0, 0, 0);
    long nback = len - cur;
    char bs[64];
    long k = nback < 63 ? nback : 63;
    for (long i = 0; i < k; i++) bs[i] = '\b';
    usys6(SYS_write, 1, (long)bs, k, 0, 0, 0);
}

static void msh_history_push(const char *line)
{
    if (line[0] == 0) return;
    /* Drop duplicate of last entry. */
    if (msh_hcount > 0) {
        int last = (msh_hcount - 1) % MSH_HIST_MAX;
        if (msh_streq(msh_history[last], line)) return;
    }
    int slot = msh_hcount % MSH_HIST_MAX;
    long i = 0;
    while (i < MSH_LINE_MAX - 1 && line[i]) { msh_history[slot][i] = line[i]; i++; }
    msh_history[slot][i] = 0;
    msh_hcount++;
}

/* D3: full-featured line editor. Returns bytes read (excluding newline) or 0 on EOF. */
static long msh_readline(char *buf, long cap)
{
    long len = 0, cur = 0;
    buf[0] = 0;
    msh_hidx = -1;
    msh_draft[0] = 0;

    enum { ST_IDLE, ST_ESC, ST_CSI } st = ST_IDLE;

    for (;;) {
        char c = 0;
        long r = usys6(SYS_read, 0, (long)&c, 1, 0, 0, 0);
        if (r == 0)
            return len > 0 ? len : 0; /* EOF */
        if (r < 0) {
            usys6(SYS_yield, 0, 0, 0, 0, 0, 0);
            continue;
        }

        if (st == ST_ESC) {
            if (c == '[') { st = ST_CSI; continue; }
            st = ST_IDLE;
        } else if (st == ST_CSI) {
            st = ST_IDLE;
            if (c == 'A') {  /* up — older history */
                if (msh_hcount == 0) continue;
                if (msh_hidx == -1) {
                    long i = 0;
                    while (i < MSH_LINE_MAX - 1 && buf[i]) { msh_draft[i] = buf[i]; i++; }
                    msh_draft[i] = 0;
                    msh_hidx = msh_hcount - 1;
                } else if (msh_hidx > 0) {
                    msh_hidx--;
                } else {
                    continue;
                }
                goto history_load;
            } else if (c == 'B') {  /* down — newer history */
                if (msh_hidx == -1) continue;
                if (msh_hidx < msh_hcount - 1) {
                    msh_hidx++;
                    goto history_load;
                }
                /* step past last → restore draft */
                msh_hidx = -1;
                long i = 0;
                while (i < MSH_LINE_MAX - 1 && msh_draft[i]) { buf[i] = msh_draft[i]; i++; }
                buf[i] = 0;
                len = i; cur = len;
                msh_redraw(buf, cur, len);
                continue;
            } else if (c == 'C') {  /* right */
                if (cur < len) { cur++; usys6(SYS_write, 1, (long)"\033[C", 3, 0, 0, 0); }
                continue;
            } else if (c == 'D') {  /* left */
                if (cur > 0) { cur--; usys6(SYS_write, 1, (long)"\b", 1, 0, 0, 0); }
                continue;
            } else {
                continue;
            }
        }

        /* raw char handling */
        if (c == 0x1B) { st = ST_ESC; continue; }
        if (c == '\n' || c == '\r') {
            usys6(SYS_write, 1, (long)"\n", 1, 0, 0, 0);
            buf[len] = 0;
            msh_history_push(buf);
            return len;
        }
        if (c == 0x01) {  /* Ctrl+A → line start */
            while (cur > 0) { cur--; usys6(SYS_write, 1, (long)"\b", 1, 0, 0, 0); }
            continue;
        }
        if (c == 0x05) {  /* Ctrl+E → line end */
            while (cur < len) { usys6(SYS_write, 1, (long)"\033[C", 3, 0, 0, 0); cur++; }
            continue;
        }
        if (c == 0x17) {  /* Ctrl+W → delete word back */
            long new_cur = cur;
            while (new_cur > 0 && buf[new_cur - 1] == ' ') new_cur--;
            while (new_cur > 0 && buf[new_cur - 1] != ' ') new_cur--;
            if (new_cur < cur) {
                long del = cur - new_cur;
                long mvlen = len - cur;
                for (long k = 0; k <= mvlen; k++) buf[new_cur + k] = buf[cur + k];
                len -= del; cur = new_cur;
                msh_redraw(buf, cur, len);
            }
            continue;
        }
        if (c == 0x15) {  /* Ctrl+U → clear line */
            buf[0] = 0; len = 0; cur = 0;
            msh_redraw(buf, cur, len);
            continue;
        }
        if (c == 0x03) {  /* Ctrl+C — leave line intact, signal SIGINT to shell */
            usys6(SYS_write, 1, (long)"^C\n", 3, 0, 0, 0);
            buf[0] = 0;
            return 0;
        }
        if (c == '\b' || c == 0x7F) {  /* backspace / DEL */
            if (cur == 0) continue;
            long mvlen = len - cur;
            for (long k = 0; k <= mvlen; k++) buf[cur - 1 + k] = buf[cur + k];
            len--; cur--;
            msh_redraw(buf, cur, len);
            continue;
        }
        if (c < 0x20 || c >= 0x7F) continue;
        if (len + 1 >= cap) continue;
        /* insert at cur */
        long mvlen = len - cur;
        for (long k = mvlen; k >= 0; k--) buf[cur + 1 + k] = buf[cur + k];
        buf[cur] = c;
        len++; cur++;
        msh_redraw(buf, cur, len);
        continue;

    history_load: {
        int slot = msh_hidx % MSH_HIST_MAX;
        long i = 0;
        while (i < MSH_LINE_MAX - 1 && msh_history[slot][i]) { buf[i] = msh_history[slot][i]; i++; }
        buf[i] = 0;
        len = i; cur = len;
        msh_redraw(buf, cur, len);
        continue;
    }
    }
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
    const char *path = argc > 1 ? argv[1] : ".";
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
    msh_write("msh: HelixOS shell (M20)\n");
    msh_write("  echo | cat | ls | cd | pwd | exit | help\n");
    msh_write("  alias | unalias | export | unset | test | [ ] | type\n");
    msh_write("  pipelines: cmd1 | cmd2 | ...\n");
    return 0;
}

static int bi_pwd(void)
{
    char buf[256];
    long r = usys6(SYS_getcwd, (long)buf, 256, 0, 0, 0, 0);
    if (r < 0) {
        msh_write("pwd: failed\n");
        return 1;
    }
    msh_write(buf);
    msh_write("\n");
    return 0;
}

static int bi_cd(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    long r = usys6(SYS_chdir, (long)path, 0, 0, 0, 0, 0);
    if (r < 0) {
        msh_write("cd: failed\n");
        return 1;
    }
    return 0;
}

/* --- M20 builtins: alias, unalias, export, unset, test/[ , type --- */

#define MSH_ALIAS_MAX 16
#define MSH_NAME_MAX  32
#define MSH_VAL_MAX   64
#define MSH_ENV_MAX   32

static struct { char name[MSH_NAME_MAX]; char value[MSH_VAL_MAX]; } msh_aliases[MSH_ALIAS_MAX];
static struct { char name[MSH_NAME_MAX]; char value[MSH_VAL_MAX]; } msh_envtab[MSH_ENV_MAX];

static void msh_copy_token(char *dst, const char *src, int cap)
{
    int n = 0;
    while (src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

/* alias [name[=value] ...] — list or define */
static int bi_alias(int argc, char **argv)
{
    int defined = 0;
    for (int i = 1; i < argc; i++) {
        /* find '=' */
        char *eq = 0;
        for (char *p = argv[i]; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) {
            /* print existing */
            int found = 0;
            for (int k = 0; k < MSH_ALIAS_MAX; k++) {
                if (msh_aliases[k].name[0] &&
                    msh_streq(msh_aliases[k].name, argv[i])) {
                    msh_write("alias ");
                    msh_write(msh_aliases[k].name);
                    msh_write("='");
                    msh_write(msh_aliases[k].value);
                    msh_write("'\n");
                    found = 1;
                    break;
                }
            }
            if (!found) { msh_write("alias: not found: "); msh_write(argv[i]); msh_write("\n"); }
            continue;
        }
        /* define: name=body */
        int slot = -1;
        for (int k = 0; k < MSH_ALIAS_MAX; k++) {
            if (!msh_aliases[k].name[0]) { slot = k; break; }
            /* replace if same name */
            char tmp[MSH_NAME_MAX];
            int nl = (int)(eq - argv[i]);
            if (nl >= MSH_NAME_MAX) nl = MSH_NAME_MAX - 1;
            for (int j = 0; j < nl; j++) tmp[j] = argv[i][j];
            tmp[nl] = 0;
            if (msh_streq(msh_aliases[k].name, tmp)) { slot = k; break; }
        }
        if (slot < 0) { msh_write("alias: table full\n"); continue; }
        int nl = (int)(eq - argv[i]);
        if (nl >= MSH_NAME_MAX) nl = MSH_NAME_MAX - 1;
        for (int j = 0; j < nl; j++) msh_aliases[slot].name[j] = argv[i][j];
        msh_aliases[slot].name[nl] = 0;
        msh_copy_token(msh_aliases[slot].value, eq + 1, MSH_VAL_MAX);
        defined++;
    }
    if (argc == 1) {
        for (int k = 0; k < MSH_ALIAS_MAX; k++) {
            if (!msh_aliases[k].name[0]) continue;
            msh_write("alias ");
            msh_write(msh_aliases[k].name);
            msh_write("='");
            msh_write(msh_aliases[k].value);
            msh_write("'\n");
            defined++;
        }
    }
    return defined > 0 ? 0 : 1;
}

static int bi_unalias(int argc, char **argv)
{
    if (argc < 2) { msh_write("unalias: need name\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        for (int k = 0; k < MSH_ALIAS_MAX; k++) {
            if (msh_aliases[k].name[0] && msh_streq(msh_aliases[k].name, argv[i])) {
                msh_aliases[k].name[0] = 0;
                msh_aliases[k].value[0] = 0;
            }
        }
    }
    return 0;
}

static int bi_export(int argc, char **argv)
{
    int n = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = 0;
        for (char *p = argv[i]; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) { msh_write("export: need NAME=VALUE\n"); continue; }
        int slot = -1;
        for (int k = 0; k < MSH_ENV_MAX; k++) {
            if (!msh_envtab[k].name[0]) { slot = k; break; }
            char tmp[MSH_NAME_MAX];
            int nl = (int)(eq - argv[i]);
            if (nl >= MSH_NAME_MAX) nl = MSH_NAME_MAX - 1;
            for (int j = 0; j < nl; j++) tmp[j] = argv[i][j];
            tmp[nl] = 0;
            if (msh_streq(msh_envtab[k].name, tmp)) { slot = k; break; }
        }
        if (slot < 0) { msh_write("export: env table full\n"); continue; }
        int nl = (int)(eq - argv[i]);
        if (nl >= MSH_NAME_MAX) nl = MSH_NAME_MAX - 1;
        for (int j = 0; j < nl; j++) msh_envtab[slot].name[j] = argv[i][j];
        msh_envtab[slot].name[nl] = 0;
        msh_copy_token(msh_envtab[slot].value, eq + 1, MSH_VAL_MAX);
        n++;
    }
    return n > 0 ? 0 : 1;
}

static int bi_unset(int argc, char **argv)
{
    if (argc < 2) { msh_write("unset: need name\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        for (int k = 0; k < MSH_ENV_MAX; k++) {
            if (msh_envtab[k].name[0] && msh_streq(msh_envtab[k].name, argv[i])) {
                msh_envtab[k].name[0] = 0;
                msh_envtab[k].value[0] = 0;
            }
        }
    }
    return 0;
}

/* test / [ — supports -f FILE, -z STR, -n STR, STR = STR, NUM -eq NUM */
static int bi_test(int argc, char **argv)
{
    /* drop trailing ']' if [ form */
    if (argc > 1 && msh_streq(argv[0], "[")) {
        if (!msh_streq(argv[argc - 1], "]")) {
            msh_write("[: missing ']'\n");
            return 2;
        }
        argc--; /* drop the ] */
    }
    if (argc < 2) { msh_write("test: need expr\n"); return 2; }

    /* Check unary flag-like ops first: -z/-n/-e/-f all take exactly one arg,
     * regardless of total argc (3 = test -f FILE, 4 = [ -f FILE ]). */
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] != '\0') {
        const char *op = argv[1];
        const char *arg = argv[2];
        if (msh_streq(op, "-z")) return arg[0] == 0 ? 0 : 1;
        if (msh_streq(op, "-n")) return arg[0] != 0 ? 0 : 1;
        if (msh_streq(op, "-f") || msh_streq(op, "-e")) {
            long fd = usys6(SYS_open, (long)arg, 0, 0, 0, 0, 0);
            if (fd < 0) return 1;
            usys6(SYS_close, fd, 0, 0, 0, 0, 0);
            return 0;
        }
        /* fall through: maybe a binary op — keep going */
    }

    if (argc == 2) {
        /* unary: -e/-f FILE or -z/-n STR */
        const char *op = argv[1];
        const char *arg = argc > 2 ? argv[2] : "";
        if (msh_streq(op, "-z")) return arg[0] == 0 ? 0 : 1;
        if (msh_streq(op, "-n")) return arg[0] != 0 ? 0 : 1;
        if (msh_streq(op, "-f") || msh_streq(op, "-e")) {
            long fd = usys6(SYS_open, (long)arg, 0, 0, 0, 0, 0);
            if (fd < 0) return 1;
            usys6(SYS_close, fd, 0, 0, 0, 0, 0);
            return 0;
        }
        msh_write("test: unknown unary: "); msh_write(op); msh_write("\n");
        return 2;
    }

    if (argc == 3) {
        if (msh_streq(argv[1], "="))  return msh_streq(argv[2], argv[3]) ? 0 : 1;
        if (msh_streq(argv[1], "!=")) return msh_streq(argv[2], argv[3]) ? 1 : 0;
        msh_write("test: need binary expr\n");
        return 2;
    }

    if (argc == 4) {
        /* INT -eq/-ne/-lt/-gt INT */
        const char *op = argv[2];
        long a = 0, b = 0;
        int neg = 0;
        const char *pa = argv[1], *pb = argv[3];
        if (*pa == '-') { neg = 1; pa++; }
        while (*pa >= '0' && *pa <= '9') a = a * 10 + (*pa++ - '0');
        if (neg) a = -a;
        neg = 0;
        if (*pb == '-') { neg = 1; pb++; }
        while (*pb >= '0' && *pb <= '9') b = b * 10 + (*pb++ - '0');
        if (neg) b = -b;
        if (msh_streq(op, "-eq")) return a == b ? 0 : 1;
        if (msh_streq(op, "-ne")) return a != b ? 0 : 1;
        if (msh_streq(op, "-lt")) return a <  b ? 0 : 1;
        if (msh_streq(op, "-gt")) return a >  b ? 0 : 1;
        msh_write("test: unknown binary: "); msh_write(op); msh_write("\n");
        return 2;
    }
    msh_write("test: too many args\n");
    return 2;
}

/* type NAME — print builtin vs would-exec */
static int bi_type(int argc, char **argv)
{
    if (argc < 2) { msh_write("type: need name\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        const char *n = argv[i];
        int is_builtin = msh_streq(n, "echo") || msh_streq(n, "cat") ||
                         msh_streq(n, "ls") || msh_streq(n, "cd") || msh_streq(n, "pwd") ||
                         msh_streq(n, "alias") || msh_streq(n, "unalias") ||
                         msh_streq(n, "export") || msh_streq(n, "unset") ||
                         msh_streq(n, "test") || msh_streq(n, "[") ||
                         msh_streq(n, "type") || msh_streq(n, "help") ||
                         msh_streq(n, "exit");
        msh_write(is_builtin ? "builtin: " : "exec: ");
        msh_write(n);
        msh_write("\n");
    }
    return 0;
}

/* Public entry for getting an alias value (returns 0 if not found). */
int msh_lookup_alias(const char *name, char *out, int cap)
{
    for (int k = 0; k < MSH_ALIAS_MAX; k++) {
        if (msh_aliases[k].name[0] && msh_streq(msh_aliases[k].name, name)) {
            msh_copy_token(out, msh_aliases[k].value, cap);
            return 1;
        }
    }
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

/* Forward decls — `msh_exec_line` splits on ';' and calls `msh_exec_pipeline`;
 * the strtok helper is also used inside that split. */
static char *msh_strtok_r(char *str, char sep, char **save);
static int msh_exec_pipeline(char *line);

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

    /* M20: split on ';' first (statement separator), then each statement
     * splits on '|' (pipeline stages). */
    int last_status = 0;
    char *stmt_save = 0;
    char *stmt = msh_strtok_r(line, ';', &stmt_save);
    while (stmt) {
        int rc = msh_exec_pipeline(stmt);
        last_status = rc;
        stmt = msh_strtok_r(0, ';', &stmt_save);
    }
    return last_status;
}

/* strtok_r-lite: returns next token from `str` split on a single `sep` char,
 * skipping leading separators. Empty tokens are skipped (so "a;;b" yields
 * "a" then "b"). Sets *save to track position. */
static char *msh_strtok_r(char *str, char sep, char **save)
{
    char *p = str ? str : *save;
    while (*p) {
        if (*p != sep) break;
        p++;
    }
    if (!*p) { *save = p; return 0; }
    char *start = p;
    while (*p) {
        if (*p == sep) break;
        p++;
    }
    if (*p) { *p = 0; *save = p + 1; } else { *save = p; }
    return start;
}

/* Execute one pipeline (no ';') — splits on '|' and runs each stage. */
static int msh_exec_pipeline(char *line)
{
    /* skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;

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

    /* M20: expand alias for stage 0 (simple: name=value with no spaces in body).
     * If argv0[0] is a known alias, swap in the alias's argv. */
    if (argc0 > 0) {
        char aval[MSH_VAL_MAX];
        if (msh_lookup_alias(argv0[0], aval, MSH_VAL_MAX)) {
            /* Re-tokenize the alias body into argv0[0..N], shift original args. */
            char tmp[MSH_LINE_MAX];
            int n = 0;
            while (aval[n] && n < MSH_LINE_MAX - 1) { tmp[n] = aval[n]; n++; }
            int aextra = argc0 - 1;
            if (aextra > 0) {
                /* space between body and trailing args */
                tmp[n++] = ' ';
                int sn = 0;
                for (int k = 1; k < argc0; k++) {
                    int l = 0;
                    while (argv0[k][l]) l++;
                    if (n + sn + l + 2 < MSH_LINE_MAX) {
                        for (int j = 0; j < l; j++) tmp[n + sn + j] = argv0[k][j];
                        sn += l;
                        if (k < argc0 - 1) {
                            tmp[n + sn] = ' ';
                            sn++;
                        }
                    }
                }
                tmp[n + sn] = 0;
            } else {
                tmp[n] = 0;
            }
            char **new_argv = (char *[MSH_ARGV_MAX]){0};
            int new_argc = msh_tokenize(tmp, new_argv);
            /* Copy back into argv0 slot. */
            for (int k = 0; k <= new_argc; k++) argv0[k] = new_argv[k];
            argc0 = new_argc;
            stages[0] = tmp;
        }
    }
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
        if (msh_streq(argv0[0], "cd"))   return bi_cd(argc0, argv0);
        if (msh_streq(argv0[0], "pwd"))  return bi_pwd();
        /* M20: extended builtins */
        if (msh_streq(argv0[0], "alias"))   return bi_alias(argc0, argv0);
        if (msh_streq(argv0[0], "unalias")) return bi_unalias(argc0, argv0);
        if (msh_streq(argv0[0], "export"))  return bi_export(argc0, argv0);
        if (msh_streq(argv0[0], "unset"))   return bi_unset(argc0, argv0);
        if (msh_streq(argv0[0], "type"))    return bi_type(argc0, argv0);
        if (msh_streq(argv0[0], "test"))    return bi_test(argc0, argv0);
        if (msh_streq(argv0[0], "["))       return bi_test(argc0, argv0);
    }
    for (int i = 1; i < nstages; i++) {
        char **av = argv_list[i] = (char *[MSH_ARGV_MAX]){0};
        msh_tokenize(stages[i], av);
    }
    return run_pipeline(stages, nstages, argv_list);
}

void msh_main(int argc, char **argv)
{
    msh_write("HelixOS msh (M20) — fork/exec/waitpid/pipe/cwd shell\n");

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
