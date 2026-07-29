#include "helix/fs.h"
#include "helix/blk.h"
#include "helix/fat.h"
#include "helix/ramfs.h"
#include "helix/vfs.h"
#include "helix/kprintf.h"
#include "helix/string.h"

/* GPT header / entry (UEFI) */
struct gpt_header {
    char     sig[8];
    u32      rev;
    u32      hdr_size;
    u32      hdr_crc;
    u32      reserved;
    u64      my_lba;
    u64      alt_lba;
    u64      first_usable;
    u64      last_usable;
    u8       disk_guid[16];
    u64      part_lba;
    u32      num_parts;
    u32      part_ent_size;
    u32      part_arr_crc;
} __attribute__((packed));

struct gpt_entry {
    u8  type_guid[16];
    u8  uniq_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attrs;
    u16 name[36];
} __attribute__((packed));

/* ESP type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B little-endian bytes */
static const u8 ESP_TYPE[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

int fs_init(void)
{
    if (blk_init() != 0) {
        kprintf("[fs] blk_init failed — no disk FS\n");
        return -1;
    }

    u8 sector[512];
    if (blk_read(1, 1, sector) != 0) {
        kprintf("[fs] read GPT header failed\n");
        return -1;
    }
    struct gpt_header *h = (struct gpt_header *)sector;
    if (memcmp(h->sig, "EFI PART", 8) != 0) {
        kprintf("[fs] no GPT signature\n");
        return -1;
    }
    kprintf("[fs] GPT ok parts=%u ent_size=%u part_lba=%llu\n",
            h->num_parts, h->part_ent_size,
            (unsigned long long)h->part_lba);

    u64 part_lba = 0, part_sectors = 0;
    u32 n = h->num_parts;
    if (n > 128)
        n = 128;
    u32 esz = h->part_ent_size;
    if (esz < 128)
        esz = 128;

    /* read partition entries starting at h->part_lba (usually 2) */
    u32 bytes = n * esz;
    u32 nsec = (bytes + 511) / 512;
    /* read up to 32 sectors (128*128) */
    if (nsec > 32)
        nsec = 32;

    static u8 ents[32 * 512];
    if (blk_read(h->part_lba, nsec, ents) != 0) {
        kprintf("[fs] read GPT entries failed\n");
        return -1;
    }

    for (u32 i = 0; i < n; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(ents + i * esz);
        int empty = 1;
        for (int k = 0; k < 16; k++) {
            if (e->type_guid[k]) {
                empty = 0;
                break;
            }
        }
        if (empty)
            continue;
        if (memcmp(e->type_guid, ESP_TYPE, 16) == 0) {
            part_lba = e->first_lba;
            part_sectors = e->last_lba - e->first_lba + 1;
            kprintf("[fs] ESP at LBA %llu size %llu sectors\n",
                    (unsigned long long)part_lba,
                    (unsigned long long)part_sectors);
            break;
        }
    }

    if (!part_lba) {
        /* fallback: first non-empty partition */
        for (u32 i = 0; i < n; i++) {
            struct gpt_entry *e = (struct gpt_entry *)(ents + i * esz);
            if (e->first_lba && e->last_lba >= e->first_lba) {
                part_lba = e->first_lba;
                part_sectors = e->last_lba - e->first_lba + 1;
                kprintf("[fs] using partition0 LBA %llu\n",
                        (unsigned long long)part_lba);
                break;
            }
        }
    }
    if (!part_lba) {
        kprintf("[fs] no partition found\n");
        return -1;
    }

    if (fat_mount(part_lba, part_sectors) != 0) {
        kprintf("[fs] fat_mount failed\n");
        return -1;
    }
    vfs_mount_root(fat_vfs_ops());
    if (ramfs_init() == 0)
        vfs_mount_tmp(ramfs_vfs_ops());
    kprintf("[fs] ready (FAT / RO + ramfs /tmp RW)\n");
    return 0;
}
