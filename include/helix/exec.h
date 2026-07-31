#pragma once

#include "helix/types.h"
#include "helix/elf.h"

/* Create user task from ELF path or image with Linux-style argv on stack.
 * argv is NULL-terminated kernel string array; copied into user stack. */
struct task *task_exec_elf(const char *name, const void *elf_img, u64 elf_size,
                           const char *const argv[]);
struct task *task_exec_path(const char *name, const char *path,
                            const char *const argv[]);

/* Build the Linux user stack: argc/argv/envp/auxv. Returns final rsp. */
u64 setup_user_stack(u64 stack_top, const char *const argv[],
                     struct elf_load_info *info, const char *execfn);

/* Run a sequence of single-task programs for smoke-linux; returns when done. */
void linux_compat_run_smoke(void);
void dyn_compat_run_smoke(void);
void musl_compat_run_smoke(void);
void m5_then_m6_smoke(void);
void msh_compat_run_smoke(void);
