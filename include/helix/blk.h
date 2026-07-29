#pragma once

#include "helix/types.h"

/* Synchronous block I/O for the boot disk (AHCI port 0). */
#define BLK_SECTOR_SIZE 512u

struct blk_dev {
    int  ready;
    u64  abar;          /* AHCI HBA MMIO base */
    u64  port_mm;       /* port 0 MMIO */
    u64  nsectors;      /* 0 if unknown */
    void *cmd_list;     /* 1K-aligned command list */
    void *fis_base;     /* 256-aligned received FIS */
    void *cmd_table;    /* command table + PRDT */
};

int  blk_init(void);
int  blk_read(u64 lba, u32 count, void *buf);
int  blk_write(u64 lba, u32 count, const void *buf);
int  blk_ready(void);
