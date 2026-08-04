#pragma once

#include "helix/types.h"

/* User VA policy:
 * - Low classic Linux loads (0x400000+) share CR3 with kernel identity; those
 *   pages get U=1 when loading ET_EXEC like BusyBox.
 * - Helix freestanding tests use [USER_BASE, USER_STACK_TOP).
 * - user_ptr_ok accepts either region.
 */
#define USER_BASE       0x0000000040000000ULL
#define USER_STACK_TOP  0x0000000044000000ULL  /* 64 MiB Helix user window */
#define USER_STACK_SIZE (256u * 1024u)
#define USER_LOW_MIN    0x0000000000400000ULL  /* classic ET_EXEC min we care about */
/* BusyBox musl static: text@0x400000 data@0x711fe0 memsz 2MiB → BSS to ~0x912000 */
#define USER_LOW_MAX    0x0000000000A00000ULL  /* 10MiB covers BB data+BSS + small brk */

/* Linux x86_64 syscall numbers (subset). */
#define SYS_read              0
#define SYS_write             1
#define SYS_open              2
#define SYS_close             3
#define SYS_fstat             5
#define SYS_brk              12
#define SYS_ioctl            16
#define SYS_yield            24
#define SYS_dup2             33
#define SYS_getpid           39
#define SYS_connect          42
#define SYS_accept           43
#define SYS_sendmsg          46
#define SYS_recvmsg          47
#define SYS_listen           50
#define SYS_setsockopt       54
#define SYS_getsockopt       55
#define SYS_fork             57
#define SYS_execve           59
#define SYS_exit             60
#define SYS_wait4            61
#define SYS_kill             62
#define SYS_uname            63
#define SYS_fcntl            72
#define SYS_getcwd           79
#define SYS_chdir            80
#define SYS_mkdir            83
#define SYS_arch_prctl      158
#define SYS_getdents64      217
#define SYS_exit_group      231
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_set_tid_address 218
#define SYS_clock_gettime   228
#define SYS_mmap             9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_socket          41
#define SYS_sendto          44
#define SYS_recvfrom        45
#define SYS_bind            49
#define SYS_pipe            22
#define SYS_readv           19
#define SYS_getrandom      318
#define SYS_fb_info        546
#define SYS_readkey        547
#define SYS_mouse_read     548

void     syscall_init(void);
int      user_ptr_ok(const void *ptr, u64 len);

/* Linux utsname (65-byte fields, fixed). */
struct helix_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
