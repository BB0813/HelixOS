#pragma once

#include "helix/types.h"

/* Scan GPT on boot disk, find ESP, mount FAT, vfs_mount_root. */
int fs_init(void);
