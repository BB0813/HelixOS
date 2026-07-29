#pragma once

/* Linux x86_64 errno values (negative returns from syscall). */
#define EPERM        1
#define ENOENT       2
#define EIO          5
#define EBADF        9
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENOSPC      28
#define ERANGE      34
#define ENOSYS      38

#define ERR(_e) (-(long)(_e))
