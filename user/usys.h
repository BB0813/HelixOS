/* HelixOS userland helpers — freestanding, no libc */
#pragma once

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_yield   24
#define SYS_exit    60
#define SYS_uname   63
#define SYS_fork    57
#define SYS_execve  59

static inline long usyscall(long nr, long a0, long a1, long a2)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sys_write(int fd, const void *buf, unsigned long n)
{
    return usyscall(SYS_write, fd, (long)buf, (long)n);
}

static inline long sys_yield(void)
{
    return usyscall(SYS_yield, 0, 0, 0);
}

static inline void sys_exit(int code)
{
    usyscall(SYS_exit, code, 0, 0);
    for (;;)
        ;
}

static inline long sys_fork(void)
{
    return usyscall(SYS_fork, 0, 0, 0);
}

static inline long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return usyscall(SYS_execve, (long)path, (long)argv, (long)envp);
}

static inline unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void uwrite(const char *s)
{
    sys_write(1, s, ustrlen(s));
}
