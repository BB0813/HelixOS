#pragma once

#include "helix/types.h"

/* FAT12/16/32 read-only on a block partition (LBA offset). */
int fat_mount(u64 part_lba, u64 part_sectors);
/* After mount, register VFS ops via fat_vfs_ops(). */
const struct vfs_ops *fat_vfs_ops(void);
int fat_is_mounted(void);
