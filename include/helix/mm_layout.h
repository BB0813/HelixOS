#pragma once

#include "helix/types.h"

/* HelixOS user address space layout (centralized from syscall.h + scattered constants).
 *
 * Virtual address regions (all in canonical lower half):
 *
 * [0x0000000000400000, 0x0000000000A00000)  USER_LOW  — classic Linux ET_EXEC
 *   BusyBox musl static: text@0x400000 data@0x711FE0 BSS to ~0x912000 + brk
 *   These pages are shared with kernel identity map (CR3) and get U=1 on load.
 *
 * [0x0000000040000000, 0x0000000044000000)  USER_BASE..USER_STACK_TOP
 *   Helix freestanding window (init, task2, helixbox, msh, tui, ld-helix apps).
 *   64 MiB total; top 1 MiB reserved for user stack.
 *
 * [0x0000000050000000, 0x0000000051000000)  INTERP window
 *   Dynamic linker / ld-musl / ld-helix mapped here (32 MiB, not overlapping USER).
 *
 * Stack grows downward from USER_STACK_TOP:
 *   Task 1 (init):      stack at USER_STACK_TOP - 1*USER_STACK_SIZE
 *   Task 2 (task2):     stack at USER_STACK_TOP - 2*USER_STACK_SIZE
 *   M6 dyn smoke:       stack at USER_STACK_TOP - USER_STACK_SIZE
 *   M6 musl smoke:      stack at USER_STACK_TOP - 2*USER_STACK_SIZE
 *
 * Bump allocators (inside sys_mmap):
 *   Helix window:  anon_bump starts at USER_BASE + 0x2000000 (0x4200000)
 *   Classic low:   anon_bump starts at USER_LOW_MIN (if addr==0)
 *
 * Notes:
 * - user_ptr_ok() in syscall.c validates against these regions.
 * - fb_map_user (fd=-4) uses fb_bump starting at USER_BASE + 0x1000000.
 * - vmm_copy_user_page_tables walks [USER_BASE, USER_STACK_TOP) only.
 */

#define USER_BASE       0x0000000040000000ULL
#define USER_STACK_TOP  0x0000000044000000ULL  /* 64 MiB Helix user window */
#define USER_STACK_SIZE (256u * 1024u)
#define USER_LOW_MIN    0x0000000000400000ULL  /* classic ET_EXEC min we care about */
#define USER_LOW_MAX    0x0000000000A00000ULL  /* 10MiB covers BB data+BSS + small brk */

/* Dynamic linker / interpreter window (not overlapping USER_BASE window) */
#define INTERP_BASE     0x0000000050000000ULL
#define INTERP_MAX      0x0000000051000000ULL  /* 32 MiB for ld-helix / ld-musl */
