/* Helixbox multi-call applets — Linux x86_64 syscall ABI (BusyBox stand-in). */
#include "usys.h"

#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_yield      24
#define SYS_uname      63
#define SYS_mkdir      83
#define SYS_getdents64 217
#define SYS_exit_group 231
#define SYS_socket      41
#define SYS_sendto      44
#define SYS_recvfrom    45
#define SYS_bind        49
#define SYS_fork        57
#define SYS_wait4       61
#define SYS_pipe        22
#define SYS_dup2        33
#define SYS_execve      59
#define SYS_exit        60
#define SYS_getcwd      79
#define SYS_chdir       80
#define WNOHANG          1

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

/* 6-arg syscall (sendto, recvfrom): a3→r10, a4→r8, a5→r9 */
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

    /* M12: cwd + chdir + relative open self-test */
    {
        char gbuf[64];
        long r = usys6(SYS_getcwd, (long)gbuf, 64, 0, 0, 0, 0);
        int ok = (r >= 0 && gbuf[0] == '/' && gbuf[1] == 0);
        if (!ok) {
            xwrite("HelixCwdFAIL getcwd root\n");
        } else {
            usys(SYS_mkdir, (long)"/tmp/cwdtest", 0755, 0);
            r = usys(SYS_chdir, (long)"/tmp/cwdtest", 0, 0);
            if (r < 0) {
                xwrite("HelixCwdFAIL chdir\n");
            } else {
                r = usys6(SYS_getcwd, (long)gbuf, 64, 0, 0, 0, 0);
                const char *exp = "/tmp/cwdtest";
                int match = 1;
                for (int i = 0; exp[i]; i++)
                    if (gbuf[i] != exp[i]) match = 0;
                if (!match || gbuf[12] != 0) {
                    xwrite("HelixCwdFAIL getcwd tmp\n");
                } else {
                    long fd = usys(SYS_open, (long)"rel.txt",
                                   (long)(O_WRONLY | O_CREAT | O_TRUNC), 0);
                    if (fd < 0) {
                        xwrite("HelixCwdFAIL rel open\n");
                    } else {
                        const char *msg = "cwd_rel_ok\n";
                        long n = 0; while (msg[n]) n++;
                        usys(SYS_write, fd, (long)msg, n);
                        usys(SYS_close, fd, 0, 0);
                        usys(SYS_chdir, (long)"/", 0, 0);
                        fd = usys(SYS_open, (long)"/tmp/cwdtest/rel.txt", 0, 0);
                        if (fd < 0) {
                            xwrite("HelixCwdFAIL abs reopen\n");
                        } else {
                            char rbuf[16];
                            long nr = usys(SYS_read, fd, (long)rbuf, 16);
                            usys(SYS_close, fd, 0, 0);
                            if (nr >= 10 && rbuf[0]=='c' && rbuf[1]=='w' && rbuf[2]=='d')
                                xwrite("HelixCwdOK\n");
                            else
                                xwrite("HelixCwdFAIL content\n");
                        }
                    }
                }
            }
        }
    }

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

    /* M8: UDP echo self-test (local loopback) */
    {
        /* AF_INET=2, SOCK_DGRAM=2, UDP=17 */
        long sfd = usys(SYS_socket, 2, 2, 17);
        if (sfd < 0) {
            xwrite("socket fail\n");
            goto done;
        }
        /* sockaddr_in: family=2, port=0x3930 (14640 big-endian), addr=127.0.0.1 BE = 0x0100007f */
        unsigned char sa[16];
        sa[0] = 2; sa[1] = 0;          /* AF_INET */
        sa[2] = 0x39; sa[3] = 0x30;     /* port 14640 BE */
        sa[4] = 127; sa[5] = 0; sa[6] = 0; sa[7] = 1; /* 127.0.0.1 BE */
        sa[8] = sa[9] = sa[10] = sa[11] = sa[12] = sa[13] = sa[14] = sa[15] = 0;
        long r = usys(SYS_bind, sfd, (long)sa, 16);
        if (r < 0) {
            xwrite("bind fail\n");
            goto done_sock;
        }
        /* sendto: "HelixNetOK" to 127.0.0.1:14640 */
        const char *udp_payload = "HelixNetOK";
        unsigned char rsa[16];
        rsa[0] = 2; rsa[1] = 0;
        rsa[2] = 0x39; rsa[3] = 0x30;
        rsa[4] = 127; rsa[5] = 0; rsa[6] = 0; rsa[7] = 1;
        rsa[8] = rsa[9] = rsa[10] = rsa[11] = rsa[12] = rsa[13] = rsa[14] = rsa[15] = 0;
        long slen = 0;
        while (udp_payload[slen]) slen++;
        r = usys6(SYS_sendto, sfd, (long)udp_payload, slen, 0, (long)rsa, 16);
        if (r >= 0) {
            /* recvfrom (expect local echo) */
            char rbuf[64];
            long nr = usys6(SYS_recvfrom, sfd, (long)rbuf, 64, 0, 0, 0);
            if (nr > 0) {
                /* check that content starts with "HelixNetOK" */
                int ok = 1;
                const char *expect = "HelixNetOK";
                for (long i = 0; i < nr && i < 10; i++)
                    if (rbuf[i] != expect[i]) ok = 0;
                if (ok)
                    xwrite("user_udp_ok\n");
            }
        }
done_sock:
        usys(SYS_close, sfd, 0, 0);
    }
    /* M10: fork self-test */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* child */
            xwrite("ForkChildOK\n");
            sys_exit(0);
        } else if (pid > 0) {
            /* parent: yield to let child run, then print */
            sys_yield();
            xwrite("ForkParentOK\n");
        } else {
            xwrite("fork_fail\n");
        }
    }

    /* M11: pipe + wait4 self-test */
    {
        int fds[2];
        if (usys(SYS_pipe, (long)fds, 0, 0) != 0) {
            xwrite("pipe_fail\n");
            goto host_udp;
        }
        long pid = sys_fork();
        if (pid == 0) {
            /* child: close read end, write to pipe */
            usys(SYS_close, fds[0], 0, 0);
            const char *msg = "PipeWriteOK";
            long n = 0;
            while (msg[n]) n++;
            long off = 0;
            while (off < n) {
                long w = usys(SYS_write, fds[1], (long)(msg + off), n - off);
                if (w > 0) {
                    off += w;
                } else {
                    usys(SYS_yield, 0, 0, 0);
                }
            }
            usys(SYS_close, fds[1], 0, 0);
            sys_exit(7); /* exit code 7 */
        } else if (pid > 0) {
            /* parent: close write end, read from pipe (loop on EAGAIN) */
            usys(SYS_close, fds[1], 0, 0);
            char rbuf[32];
            long nr = 0;
            for (;;) {
                long r = usys(SYS_read, fds[0], (long)(rbuf + nr), 32 - nr);
                if (r > 0) {
                    nr += r;
                    break; /* small payload arrives in one shot */
                } else if (r == 0) {
                    break; /* EOF */
                } else {
                    usys(SYS_yield, 0, 0, 0);
                }
            }
            int ok = (nr == 11); /* "PipeWriteOK" is 11 bytes */
            if (ok) {
                const char *expect = "PipeWriteOK";
                for (long i = 0; i < nr && i < 11; i++)
                    if (rbuf[i] != expect[i]) { ok = 0; break; }
            }
            usys(SYS_close, fds[0], 0, 0);
            if (ok)
                xwrite("PipeOK\n");
            else
                xwrite("PipeFAIL\n");
            /* wait for child, check exit status */
            int status = 0;
            long wpid = usys6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
            if (wpid == pid && ((status >> 8) & 0xFF) == 7)
                xwrite("WaitOK\n");
            else
                xwrite("WaitFAIL\n");
        } else {
            xwrite("pipe_fork_fail\n");
        }
    }

host_udp:

    /* M8: host ↔ guest UDP ping via QEMU port forward */
    {
        /* AF_INET=2, SOCK_DGRAM=2, UDP=17 */
        long sfd = usys(SYS_socket, 2, 2, 17);
        if (sfd < 0) {
            xwrite("host_udp: socket fail\n");
            goto done;
        }
        /* Bind on port 12345 (0x3039 BE) */
        unsigned char bsa[16];
        for (int i = 0; i < 16; i++) bsa[i] = 0;
        bsa[0] = 2; bsa[1] = 0;       /* AF_INET */
        bsa[2] = 0x30; bsa[3] = 0x39;  /* port 12345 BE */
        long r = usys(SYS_bind, sfd, (long)bsa, 16);
        if (r < 0) {
            xwrite("host_udp: bind fail\n");
            usys(SYS_close, sfd, 0, 0);
            goto done;
        }
        /* Destination: 10.0.2.2:12345 (QEMU host forward) */
        unsigned char dsa[16];
        for (int i = 0; i < 16; i++) dsa[i] = 0;
        dsa[0] = 2; dsa[1] = 0;       /* AF_INET */
        dsa[2] = 0x30; dsa[3] = 0x39;  /* port 12345 BE */
        dsa[4] = 10; dsa[5] = 2; dsa[6] = 0; dsa[7] = 2; /* 10.0.2.2 BE */
        const char *ping = "HELIX_PING";
        long plen = 0;
        while (ping[plen]) plen++;
        r = usys6(SYS_sendto, sfd, (long)ping, plen, 0, (long)dsa, 16);
        if (r < 0) {
            xwrite("host_udp: send fail\n");
            usys(SYS_close, sfd, 0, 0);
            goto done;
        }
        /* Poll recvfrom with yield (cooperative wait) */
        char rbuf[64];
        int ok = 0;
        for (int attempt = 0; attempt < 200; attempt++) {
            long nr = usys6(SYS_recvfrom, sfd, (long)rbuf, 64, 0, 0, 0);
            if (nr > 0) {
                /* Check reply starts with "ECHO:HELIX_PING" */
                const char *expect = "ECHO:HELIX_PING";
                ok = 1;
                for (long i = 0; i < nr && i < 15; i++)
                    if (rbuf[i] != expect[i]) { ok = 0; break; }
                break;
            }
            usys(SYS_yield, 0, 0, 0);
        }
        if (ok)
            xwrite("host_udp_ok\n");
        else
            xwrite("host_udp_timeout\n");
        usys(SYS_close, sfd, 0, 0);
    }

done:
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
