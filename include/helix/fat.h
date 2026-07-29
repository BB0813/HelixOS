#pragma once

#include "helix/types.h"

/* FAT12/16/32 on a block partition (LBA offset).
 * FAT16: open/create/read/write/mkdir on root (persistent via AHCI).
 * FAT32: read still supported; write/mkdir deferred.
 */
int fat_mount(u64 part_lba, u64 part_sectors);
const struct vfs_ops *fat_vfs_ops(void);
int fat_is_mounted(void);
/* Kernel self-test: create/write/readback a small root file. */
int fat_selftest_write(void);
