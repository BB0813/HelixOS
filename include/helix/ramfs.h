#pragma once

#include "helix/vfs.h"

/* In-memory writable FS mounted at /tmp (and /tmp/...). */
int ramfs_init(void);
const struct vfs_ops *ramfs_vfs_ops(void);

/* Kernel helpers */
int ramfs_mkdir(const char *path);
int ramfs_create_file(const char *path);
