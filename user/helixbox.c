/* Helixbox multi-call applets — Linux x86_64 syscall ABI (BusyBox stand-in). */
#include "usys.h"

#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_uname      63
#define SYS_mkdir      83
#define SYS_getdents64 217
#define SYS_exit_group 231

/* Linux open flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  64
#define O_TRUNC  512
#define O_APPEND 1024

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static long usys(long nr, long a0, long a1, long a2)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static void xwrite(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    usys(SYS_write, 1, (long)s, (long)(p - s));
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static const char *basename_of(const char *s)
{
    const char *b = s;
    for (; *s; s++)
        if (*s == '/')
            b = s + 1;
    return b;
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            xwrite(" ");
        xwrite(argv[i]);
    }
    xwrite("\n");
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        xwrite("cat: need file\n");
        return;
    }
    long fd = usys(SYS_open, (long)argv[1], 0, 0);
    if (fd < 0) {
        xwrite("cat: open failed\n");
        return;
    }
    char buf[128];
    for (;;) {
        long n = usys(SYS_read, fd, (long)buf, sizeof(buf));
        if (n <= 0)
            break;
        usys(SYS_write, 1, (long)buf, n);
    }
    usys(SYS_close, fd, 0, 0);
}

static void cmd_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    long fd = usys(SYS_open, (long)path, 0, 0);
    if (fd < 0) {
        xwrite("ls: open failed\n");
        return;
    }
    char buf[512];
    long n = usys(SYS_getdents64, fd, (long)buf, sizeof(buf));
    if (n < 0) {
        xwrite("ls: getdents failed\n");
        usys(SYS_close, fd, 0, 0);
        return;
    }
    long off = 0;
    while (off < n) {
        struct linux_dirent64 *de = (void *)(buf + off);
        xwrite(de->d_name);
        xwrite("\n");
        if (de->d_reclen == 0)
            break;
        off += de->d_reclen;
    }
    usys(SYS_close, fd, 0, 0);
}

static void cmd_uname(void)
{
    struct utsname u;
    if (usys(SYS_uname, (long)&u, 0, 0) != 0) {
        xwrite("uname failed\n");
        return;
    }
    xwrite(u.sysname);
    xwrite("\n");
}

static void cmd_sh(int argc, char **argv)
{
    if (argc >= 3 && streq(argv[1], "-c")) {
        const char *cmd = argv[2];
        if (starts(cmd, "echo ")) {
            xwrite(cmd + 5);
            xwrite("\n");
            return;
        }
        xwrite("sh: unsupported\n");
        return;
    }
    xwrite("sh: usage: sh -c CMD\n");
}

static void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        xwrite("mkdir: need path\n");
        return;
    }
    long r = usys(SYS_mkdir, (long)argv[1], 0755, 0);
    if (r < 0)
        xwrite("mkdir: failed\n");
}

static void cmd_smoke(void)
{
    char *e1[] = { "echo", "HelixLinuxOK", 0 };
    cmd_echo(2, e1);
    char *e2[] = { "cat", "/hello.txt", 0 };
    cmd_cat(2, e2);
    char *e3[] = { "ls", "/", 0 };
    cmd_ls(2, e3);
    cmd_uname();
    char *e4[] = { "sh", "-c", "echo sh_ok", 0 };
    cmd_sh(3, e4);

    /* writable /tmp */
    char *mk[] = { "mkdir", "/tmp/a", 0 };
    cmd_mkdir(2, mk);
    long fd = usys(SYS_open, (long)"/tmp/a/x", (long)(O_WRONLY | O_CREAT | O_TRUNC), 0);
    if (fd >= 0) {
        const char *msg = "tmp_write_ok\n";
        const char *p = msg;
        while (*p)
            p++;
        usys(SYS_write, fd, (long)msg, (long)(p - msg));
        usys(SYS_close, fd, 0, 0);
        char *c2[] = { "cat", "/tmp/a/x", 0 };
        cmd_cat(2, c2);
    } else {
        xwrite("tmp open failed\n");
    }

    xwrite("helixbox_smoke_done\n");
}

static void dispatch(int argc, char **argv)
{
    const char *app = argc > 0 ? basename_of(argv[0]) : "helixbox";
    if (streq(app, "helixbox") && argc > 1) {
        app = argv[1];
        argv++;
        argc--;
    }
    if (streq(app, "echo"))
        cmd_echo(argc, argv);
    else if (streq(app, "cat"))
        cmd_cat(argc, argv);
    else if (streq(app, "ls"))
        cmd_ls(argc, argv);
    else if (streq(app, "uname"))
        cmd_uname();
    else if (streq(app, "sh"))
        cmd_sh(argc, argv);
    else if (streq(app, "mkdir"))
        cmd_mkdir(argc, argv);
    else if (streq(app, "smoke"))
        cmd_smoke();
    else {
        xwrite("helixbox: unknown applet\n");
    }
}

/* Linux process entry: rsp -> argc, argv pointers, ...
 * No CRT — read argc/argv from the stack the kernel built. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov (%%rsp), %%rdi\n\t"       /* argc */
        "lea 8(%%rsp), %%rsi\n\t"      /* argv */
        "call helixbox_main\n\t"
        "mov %%rax, %%rdi\n\t"
        "mov $231, %%eax\n\t"          /* exit_group */
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        :
        : "memory");
}

int helixbox_main(int argc, char **argv)
{
    dispatch(argc, argv);
    return 0;
}
