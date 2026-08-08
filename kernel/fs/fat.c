#include "helix/fat.h"
#include "helix/blk.h"
#include "helix/vfs.h"
#include "helix/heap.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/types.h"

/* Read-only FAT12/16 (and basic FAT32) on a partition. */

struct fat_fs {
    int  ready;
    u64  part_lba;
    u64  part_sectors;
    u16  byts_per_sec;
    u8   sec_per_clus;
    u16  rsvd_sec_cnt;
    u8   num_fats;
    u16  root_ent_cnt;     /* 0 for FAT32 */
    u32  fatsz;            /* sectors per FAT */
    u32  totsec;
    u8   fat_type;         /* 12, 16, or 32 */
    u32  root_clus;        /* FAT32 */
    u32  first_fat_lba;    /* relative to part */
    u32  root_lba;         /* FAT12/16 root dir start (rel) */
    u32  root_sectors;
    u32  data_lba;         /* first data sector (rel) */
    u32  data_clusters;
};

struct fat_file {
    u32 start_clus;
    u64 size;
    u64 pos;
    char name83[11]; /* for dirent updates after write */
    /* D7.2: FAT dirent date/time (BCD-encoded FAT format, little-endian u16).
     * 0 if dirent had no timestamp. Used by fat_fstat to fill st_mtime etc. */
    u16 wrt_time;   /* dirent offset 22: hh:mm:ss (2sec units) */
    u16 wrt_date;   /* dirent offset 24: YYYYYYY-MMMM-DDDDD */
    u16 acc_date;   /* dirent offset 18: last access date */
    u16 crt_time;   /* dirent offset 14: creation time */
    u16 crt_date;   /* dirent offset 16: creation date */
};

/* M20: dir iterator stored in fs_priv for subdirs. Root / uses NULL. */
struct fat_dir_iter {
    u16 clus;     /* current cluster; 0 = FAT16 root region */
    u8  sec;      /* sector within cluster */
    u16 off;      /* byte offset within sector (always 0..480 multiple of 32) */
    u16 eof;      /* 1 once we hit the chain end */
};

static struct fat_fs g_fat;

/* D7.2: FAT date/time ↔ Unix conversions (forward decl — used by fill_83_dirent
 * and fat_open before the static definitions later in the file). */
static u64 fat_date_to_unix(u16 fat_date, u16 fat_time);
static void fat_unix_to_date(u64 unix_sec, u16 *out_date, u16 *out_time);

static int read_sector(u64 rel_lba, void *buf)
{
    return blk_read(g_fat.part_lba + rel_lba, 1, buf);
}

static int read_sectors(u64 rel_lba, u32 n, void *buf)
{
    return blk_read(g_fat.part_lba + rel_lba, n, buf);
}

static u32 fat_get(u32 clus)
{
    u8 sec[512];
    if (g_fat.fat_type == 32) {
        u32 off = clus * 4;
        u32 sec_i = off / g_fat.byts_per_sec;
        u32 ent = off % g_fat.byts_per_sec;
        if (read_sector(g_fat.first_fat_lba + sec_i, sec) != 0)
            return 0x0FFFFFF7;
        u32 v = sec[ent] | (sec[ent + 1] << 8) | (sec[ent + 2] << 16) | (sec[ent + 3] << 24);
        return v & 0x0FFFFFFF;
    }
    if (g_fat.fat_type == 16) {
        u32 off = clus * 2;
        u32 sec_i = off / g_fat.byts_per_sec;
        u32 ent = off % g_fat.byts_per_sec;
        if (read_sector(g_fat.first_fat_lba + sec_i, sec) != 0)
            return 0xFFF7;
        return sec[ent] | (sec[ent + 1] << 8);
    }
    /* FAT12 */
    u32 off = clus + (clus / 2);
    u32 sec_i = off / g_fat.byts_per_sec;
    u32 ent = off % g_fat.byts_per_sec;
    if (read_sector(g_fat.first_fat_lba + sec_i, sec) != 0)
        return 0xFF7;
    u16 val;
    if (ent == 511) {
        u8 sec2[512];
        if (read_sector(g_fat.first_fat_lba + sec_i + 1, sec2) != 0)
            return 0xFF7;
        val = sec[511] | (sec2[0] << 8);
    } else {
        val = sec[ent] | (sec[ent + 1] << 8);
    }
    if (clus & 1)
        return val >> 4;
    return val & 0x0FFF;
}

static int fat_is_eof(u32 clus)
{
    if (g_fat.fat_type == 32)
        return clus >= 0x0FFFFFF8;
    if (g_fat.fat_type == 16)
        return clus >= 0xFFF8;
    return clus >= 0xFF8;
}

static u32 clus_to_lba(u32 clus)
{
    return g_fat.data_lba + (clus - 2) * g_fat.sec_per_clus;
}

static void encode_83_upper(const char *name, char out[11])
{
    memset(out, ' ', 11);
    const char *dot = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.')
            dot = p;
    }
    int i = 0;
    for (const char *p = name; *p && p != dot && i < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '/')
            break;
        out[i++] = c;
    }
    if (dot) {
        i = 8;
        for (const char *p = dot + 1; *p && i < 11; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            out[i++] = c;
        }
    }
}

static int dir_name_eq(const u8 *dirent, const char want[11])
{
    return memcmp(dirent, want, 11) == 0;
}

/* Find entry in a directory cluster chain (or FAT16 root region).
 * For FAT16 root: start_clus == 0 means use root_lba/root_sectors. */
/* D7.2: FAT dirent metadata captured during find_in_dir, propagated to fat_file
 * for fstat. Fields are raw FAT u16 little-endian date/time formats. */
struct fat_dirent_meta {
    u32 clus;
    u32 size;
    u8  attr;
    u16 wrt_time;  /* offset 22 */
    u16 wrt_date;  /* offset 24 */
    u16 acc_date;  /* offset 18 */
    u16 crt_time;  /* offset 14 */
    u16 crt_date;  /* offset 16 */
};

static int find_in_dir(u32 start_clus, const char want[11], struct fat_dirent_meta *m)
{
    u8 sec[512];
    if (start_clus == 0 && g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            for (int i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return -1;
                if (e[0] == 0xE5)
                    continue;
                if (e[11] == 0x0F)
                    continue; /* LFN */
                if (dir_name_eq(e, want)) {
                    u32 cl = e[26] | (e[27] << 8);
                    if (g_fat.fat_type == 32)
                        cl |= (u32)(e[20] | (e[21] << 8)) << 16;
                    m->clus = cl;
                    m->size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
                    m->attr = e[11];
                    m->crt_time = e[14] | (e[15] << 8);
                    m->crt_date = e[16] | (e[17] << 8);
                    m->acc_date = e[18] | (e[19] << 8);
                    m->wrt_time = e[22] | (e[23] << 8);
                    m->wrt_date = e[24] | (e[25] << 8);
                    return 0;
                }
            }
        }
        return -1;
    }

    u32 clus = start_clus ? start_clus : g_fat.root_clus;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            if (read_sector(clus_to_lba(clus) + s, sec) != 0)
                return -1;
            for (int i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return -1;
                if (e[0] == 0xE5)
                    continue;
                if (e[11] == 0x0F)
                    continue;
                if (dir_name_eq(e, want)) {
                    u32 c = e[26] | (e[27] << 8);
                    if (g_fat.fat_type == 32)
                        c |= (u32)(e[20] | (e[21] << 8)) << 16;
                    m->clus = c;
                    m->size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
                    m->attr = e[11];
                    m->crt_time = e[14] | (e[15] << 8);
                    m->crt_date = e[16] | (e[17] << 8);
                    m->acc_date = e[18] | (e[19] << 8);
                    m->wrt_time = e[22] | (e[23] << 8);
                    m->wrt_date = e[24] | (e[25] << 8);
                    return 0;
                }
            }
        }
        clus = fat_get(clus);
    }
    return -1;
}

/* Resolve absolute path like /hello.txt or /bin/init.elf (leading / optional).
 * If out_attr is non-NULL, fills dirent's attribute byte (bit 0x10 = dir).
 * Leaf dirs resolve successfully (caller may open them for getdents64).
 * D7.2: if out_meta is non-NULL, fills full dirent metadata (date/time). */
static int fat_resolve(const char *path, u32 *out_clus, u32 *out_size, u8 *out_attr,
                       struct fat_dirent_meta *out_meta)
{
    while (*path == '/')
        path++;
    if (!*path)
        return -1; /* root itself not a file */

    u32 dir_clus = (g_fat.fat_type == 32) ? g_fat.root_clus : 0;
    char comp[64];
    for (;;) {
        int n = 0;
        while (*path && *path != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *path++;
        comp[n] = 0;
        while (*path == '/')
            path++;

        char w[11];
        encode_83_upper(comp, w);
        struct fat_dirent_meta m;
        if (find_in_dir(dir_clus, w, &m) != 0)
            return -1;
        if (*path) {
            /* must be directory */
            if (!(m.attr & 0x10))
                return -1;
            dir_clus = m.clus;
            continue;
        }
        /* leaf: report cluster + size + attr unconditionally */
        *out_clus = m.clus;
        *out_size = m.size;
        if (out_attr)
            *out_attr = m.attr;
        if (out_meta)
            *out_meta = m;
        return 0;
    }
}

static int fat_close(struct vfs_file *f)
{
    if (f->fs_priv)
        kfree(f->fs_priv);
    kfree(f);
    return 0;
}

/* linux_dirent64 layout */
struct linux_dirent64 {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8  d_type;
    char d_name[];
} __attribute__((packed));

#define DT_DIR 4
#define DT_REG 8

static void ent_to_name(const u8 *e, char name[13])
{
    int n = 0;
    for (int j = 0; j < 8 && e[j] != ' '; j++)
        name[n++] = (char)e[j];
    if (e[8] != ' ') {
        name[n++] = '.';
        for (int j = 8; j < 11 && e[j] != ' '; j++)
            name[n++] = (char)e[j];
    }
    name[n] = 0;
}

static long fat_getdents64(struct vfs_file *f, void *buf, u64 len)
{
    if (!f->is_dir)
        return -20; /* ENOTDIR */

    u8 sec[512];
    u64 produced = 0;
    u8 *out = buf;

    /* M20: dispatch on fs_priv. NULL → root directory (FAT16 fixed region or
     * FAT32 root_clus). Otherwise → subdir iterator that walks cluster chain. */
    int is_root = (f->fs_priv == 0);
    struct fat_dir_iter *it = (struct fat_dir_iter *)f->fs_priv;

    /* Single-cluster cap for root (preserves M5 behavior). For subdirs we
     * keep walking via the chain; the iterator is advanced across calls. */
    u32 root_max_entries = 0;
    if (is_root) {
        if (g_fat.fat_type != 32)
            root_max_entries = g_fat.root_ent_cnt;
        else
            root_max_entries = (g_fat.sec_per_clus * g_fat.byts_per_sec) / 32;
    }

    u64 index = f->pos;  /* entry index */
    u32 local_sec_i = 0; /* used only for root */
    u32 local_off = 0;

    while (1) {
        u8 *e;
        if (is_root) {
            if (index >= root_max_entries)
                break;
            local_sec_i = (u32)(index * 32 / 512);
            local_off = (u32)(index * 32 % 512);
            if (g_fat.fat_type != 32) {
                if (local_sec_i >= g_fat.root_sectors)
                    break;
                if (read_sector(g_fat.root_lba + local_sec_i, sec) != 0)
                    return -5;
            } else {
                if (read_sector(clus_to_lba(g_fat.root_clus) + local_sec_i, sec) != 0)
                    return -5;
            }
            e = &sec[local_off];
        } else {
            /* Walk cluster chain via iterator. */
            if (it->eof)
                break;
            if (it->clus == 0 && g_fat.fat_type != 32) {
                /* FAT16 fixed root region (unreachable for subdir but safe) */
                if (index >= root_max_entries)
                    break;
                local_sec_i = (u32)(index * 32 / 512);
                local_off = (u32)(index * 32 % 512);
                if (local_sec_i >= g_fat.root_sectors)
                    break;
                if (read_sector(g_fat.root_lba + local_sec_i, sec) != 0)
                    return -5;
                e = &sec[local_off];
            } else {
                u32 spc = g_fat.sec_per_clus;
                u32 bps = g_fat.byts_per_sec;
                if (it->off >= spc * bps) {
                    /* Advance to next cluster. */
                    u32 next = fat_get(it->clus);
                    if (fat_is_eof(next) || next < 2) {
                        it->eof = 1;
                        break;
                    }
                    it->clus = (u16)next;
                    it->sec = 0;
                    it->off = 0;
                }
                if (read_sector(clus_to_lba(it->clus) + it->sec, sec) != 0)
                    return -5;
                e = &sec[it->off];
            }
        }
        if (e[0] == 0) {
            if (!is_root)
                it->eof = 1;
            break;
        }
        index++;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
            goto advance;
        char name[13];
        ent_to_name(e, name);
        u16 namelen = (u16)strlen(name);
        u16 reclen = (u16)align_up_u64(8 + 8 + 2 + 1 + namelen + 1, 8);
        if (produced + reclen > len) {
            if (produced == 0)
                return -22; /* EINVAL buffer too small */
            break;
        }
        struct linux_dirent64 *de = (struct linux_dirent64 *)(out + produced);
        de->d_ino = index;
        de->d_off = (i64)index;
        de->d_reclen = reclen;
        de->d_type = (e[11] & 0x10) ? DT_DIR : DT_REG;
        memcpy(de->d_name, name, namelen + 1);
        produced += reclen;
        f->pos = index;
    advance:
        if (!is_root)
            it->off += 32;
        if (is_root)
            continue;
    }
    return (long)produced;
}

struct helix_stat {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize;
    i64 st_blocks;
    i64 st_atime;
    i64 st_atime_nsec;
    i64 st_mtime;
    i64 st_mtime_nsec;
    i64 st_ctime;
    i64 st_ctime_nsec;
    i64 __unused[3];
};

#define S_IFREG 0100000
#define S_IFDIR 0040000

/* D7.2: FAT date/time ↔ Unix epoch conversions.
 * FAT date: bits 15..9 = year-1980, 8..5 = month (1..12), 4..0 = day (1..31)
 * FAT time: bits 15..11 = hours (0..23), 10..5 = minutes (0..59), 4..0 = seconds/2 */
static u64 fat_date_to_unix(u16 fat_date, u16 fat_time)
{
    if (!fat_date && !fat_time)
        return 0;
    u64 day = (fat_date & 0x1F);
    u64 mon = (fat_date >> 5) & 0x0F;
    u64 yr  = 1980 + ((fat_date >> 9) & 0x7F);
    if (mon < 1 || mon > 12 || day < 1 || day > 31)
        return 0;
    /* Days from 1970-01-01 to yr-mon-01 (proleptic Gregorian, Howard Hinnant). */
    u64 y = yr - (mon <= 2);
    u64 era = (y >= 0 ? y : y - 399) / 400;
    u64 yoe = y - era * 400;
    u64 doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5;
    u64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    u64 days = era * 146097 + doe - 719468 + (day - 1);
    u64 hour = (fat_time >> 11) & 0x1F;
    u64 min  = (fat_time >> 5) & 0x3F;
    u64 sec  = (fat_time & 0x1F) * 2;
    return days * 86400 + hour * 3600 + min * 60 + sec;
}

static void fat_unix_to_date(u64 unix_sec, u16 *out_date, u16 *out_time)
{
    if (unix_sec == 0) {
        *out_date = 0;
        *out_time = 0;
        return;
    }
    /* Split unix seconds into days + h:m:s */
    u64 days = unix_sec / 86400;
    u64 rem  = unix_sec % 86400;
    u64 hour = rem / 3600;
    u64 min  = (rem % 3600) / 60;
    u64 sec  = rem % 60;

    /* Howard Hinnant civil_from_days inverse: days since 1970-01-01 → y/m/d */
    days += 719468;
    u64 era = days / 146097;
    u64 doe = days - era * 146097;           /* [0, 146096] */
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0, 399] */
    u64 y = yoe + era * 400;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  /* [0, 365] */
    u64 mp = (5 * doy + 2) / 153;            /* [0, 11] */
    u64 d  = doy - (153 * mp + 2) / 5 + 1;   /* [1, 31] */
    u64 m  = mp + (mp < 10 ? 3 : -9);        /* [1, 12] */
    y += (m <= 2);
    if (y < 1980) {
        *out_date = 0;
        *out_time = 0;
        return;
    }
    u16 yr_off = (u16)(y - 1980);
    if (yr_off > 127) yr_off = 127;  /* FAT year field is 7 bits */
    *out_date = (u16)((yr_off << 9) | (m << 5) | d);
    *out_time = (u16)((hour << 11) | (min << 5) | (sec / 2));
}

static long fat_fstat(struct vfs_file *f, void *statbuf)
{
    struct helix_stat *st = statbuf;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_blksize = 512;
    if (f->is_dir) {
        st->st_mode = S_IFDIR | 0555;
        st->st_size = 0;
    } else {
        st->st_mode = S_IFREG | 0444;
        st->st_size = (i64)f->size;
        st->st_blocks = (i64)((f->size + 511) / 512);
    }
    /* D7.2: pseudo-inode from fs_priv (fat_file.start_clus or dir_iter.clus).
     * Unique per file/dir — two paths to the same file share cluster. */
    if (f->fs_priv) {
        struct fat_file *ff = f->fs_priv;
        st->st_ino = ff->start_clus ? ff->start_clus : 1;
    } else {
        st->st_ino = 1;  /* root dir */
    }
    /* D7.2: real timestamps from FAT dirent (if present). */
    if (f->fs_priv && !f->is_dir) {
        struct fat_file *ff = f->fs_priv;
        st->st_mtime = (i64)fat_date_to_unix(ff->wrt_date, ff->wrt_time);
        st->st_atime = (i64)fat_date_to_unix(ff->acc_date, ff->wrt_time);
        st->st_ctime = (i64)fat_date_to_unix(ff->crt_date, ff->crt_time);
    }
    return 0;
}


static int fat_read_file(struct vfs_file *f, void *buf, u64 len, u64 *out_n)
{
    struct fat_file *ff = f->fs_priv;
    if (!ff)
        return -1;
    if (ff->pos >= ff->size) {
        *out_n = 0;
        return 0;
    }
    if (len > ff->size - ff->pos)
        len = ff->size - ff->pos;

    u32 clus_size = (u32)g_fat.sec_per_clus * g_fat.byts_per_sec;
    u64 remain = len;
    u8 *dst = buf;
    u64 pos = ff->pos;

    /* walk to cluster containing pos */
    u32 clus = ff->start_clus;
    u64 skip = pos / clus_size;
    while (skip--) {
        clus = fat_get(clus);
        if (fat_is_eof(clus) || clus < 2)
            return -1;
    }
    u32 off_in_clus = (u32)(pos % clus_size);

    u8 *secbuf = kmalloc(clus_size);
    if (!secbuf)
        return -1;

    while (remain) {
        if (read_sectors(clus_to_lba(clus), g_fat.sec_per_clus, secbuf) != 0) {
            kfree(secbuf);
            return -1;
        }
        u32 chunk = clus_size - off_in_clus;
        if (chunk > remain)
            chunk = (u32)remain;
        memcpy(dst, secbuf + off_in_clus, chunk);
        dst += chunk;
        remain -= chunk;
        pos += chunk;
        off_in_clus = 0;
        if (remain) {
            clus = fat_get(clus);
            if (fat_is_eof(clus) || clus < 2)
                break;
        }
    }
    kfree(secbuf);
    u64 got = len - remain;
    ff->pos += got;
    f->pos = ff->pos;
    *out_n = got;
    return 0;
}

static int fat_readdir_root(void (*cb)(const char *name, u64 size, void *user), void *user)
{
    u8 sec[512];
    char name[13];
    if (g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            for (int i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return 0;
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
                    continue;
                int n = 0;
                for (int j = 0; j < 8 && e[j] != ' '; j++)
                    name[n++] = (char)e[j];
                if (e[8] != ' ') {
                    name[n++] = '.';
                    for (int j = 8; j < 11 && e[j] != ' '; j++)
                        name[n++] = (char)e[j];
                }
                name[n] = 0;
                u32 sz = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
                cb(name, sz, user);
            }
        }
        return 0;
    }
    /* FAT32 root cluster chain — list first sector only (shell convenience) */
    u32 clus = g_fat.root_clus;
    if (read_sector(clus_to_lba(clus), sec) != 0)
        return -1;
    for (int i = 0; i < 512; i += 32) {
        u8 *e = &sec[i];
        if (e[0] == 0)
            break;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
            continue;
        int n = 0;
        for (int j = 0; j < 8 && e[j] != ' '; j++)
            name[n++] = (char)e[j];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (int j = 8; j < 11 && e[j] != ' '; j++)
                name[n++] = (char)e[j];
        }
        name[n] = 0;
        u32 sz = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
        cb(name, sz, user);
    }
    return 0;
}

/* ---- FAT16/FAT32 write (root create/write/mkdir). FAT12 write not supported. ---- */

static int fat_writable_type(void)
{
    return g_fat.fat_type == 16 || g_fat.fat_type == 32;
}

static u32 fat_eof_mark(void)
{
    return g_fat.fat_type == 32 ? 0x0FFFFFF8u : 0xFFF8u;
}

/* AHCI PRDT needs a stable identity-mapped buffer; avoid small stack temps. */
static u8 g_sec_bounce[512] __attribute__((aligned(16)));

static int write_sector(u64 rel_lba, const void *buf)
{
    memcpy(g_sec_bounce, buf, 512);
    return blk_write(g_fat.part_lba + rel_lba, 1, g_sec_bounce);
}

static int write_sectors(u64 rel_lba, u32 n, const void *buf)
{
    const u8 *p = buf;
    for (u32 i = 0; i < n; i++) {
        if (write_sector(rel_lba + i, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

static int fat_put(u32 clus, u32 val)
{
    u8 sec[512];
    if (g_fat.fat_type == 32) {
        u32 off = clus * 4;
        u32 sec_i = off / g_fat.byts_per_sec;
        u32 ent = off % g_fat.byts_per_sec;
        if (read_sector(g_fat.first_fat_lba + sec_i, sec) != 0)
            return -1;
        /* Keep top 4 bits of FAT32 entry */
        u32 old = sec[ent] | (sec[ent + 1] << 8) | (sec[ent + 2] << 16) | (sec[ent + 3] << 24);
        u32 neu = (old & 0xF0000000u) | (val & 0x0FFFFFFFu);
        sec[ent] = (u8)(neu & 0xFF);
        sec[ent + 1] = (u8)((neu >> 8) & 0xFF);
        sec[ent + 2] = (u8)((neu >> 16) & 0xFF);
        sec[ent + 3] = (u8)((neu >> 24) & 0xFF);
        for (u8 f = 0; f < g_fat.num_fats; f++) {
            if (write_sector(g_fat.first_fat_lba + f * g_fat.fatsz + sec_i, sec) != 0)
                return -1;
        }
        return 0;
    }
    if (g_fat.fat_type == 16) {
        u32 off = clus * 2;
        u32 sec_i = off / g_fat.byts_per_sec;
        u32 ent = off % g_fat.byts_per_sec;
        if (read_sector(g_fat.first_fat_lba + sec_i, sec) != 0)
            return -1;
        sec[ent] = (u8)(val & 0xFF);
        sec[ent + 1] = (u8)((val >> 8) & 0xFF);
        for (u8 f = 0; f < g_fat.num_fats; f++) {
            if (write_sector(g_fat.first_fat_lba + f * g_fat.fatsz + sec_i, sec) != 0)
                return -1;
        }
        return 0;
    }
    return -1;
}

static int zero_cluster(u32 clus)
{
    u8 z[512];
    memset(z, 0, sizeof(z));
    u32 lba = clus_to_lba(clus);
    for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
        if (write_sector(lba + s, z) != 0)
            return -1;
    }
    return 0;
}

static u32 fat_alloc_cluster(void)
{
    if (!fat_writable_type())
        return 0;
    for (u32 c = 2; c < g_fat.data_clusters + 2; c++) {
        if (fat_get(c) == 0) {
            if (fat_put(c, fat_eof_mark()) != 0)
                return 0;
            if (zero_cluster(c) != 0)
                return 0;
            return c;
        }
    }
    return 0;
}

/* Free dirent slot: FAT16 fixed root, or FAT32 root cluster chain (extend if full). */
static int dir_find_free_slot(u32 dir_clus, u64 *out_lba, u32 *out_off, u8 *sec_out)
{
    if (dir_clus == 0 && g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec_out) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                if (sec_out[i] == 0 || sec_out[i] == 0xE5) {
                    *out_lba = g_fat.root_lba + s;
                    *out_off = i;
                    return 0;
                }
            }
        }
        return -1;
    }

    u32 clus = dir_clus ? dir_clus : g_fat.root_clus;
    u32 prev = 0;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            u32 lba = clus_to_lba(clus) + s;
            if (read_sector(lba, sec_out) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                if (sec_out[i] == 0 || sec_out[i] == 0xE5) {
                    *out_lba = lba;
                    *out_off = i;
                    return 0;
                }
            }
        }
        prev = clus;
        clus = fat_get(clus);
    }
    /* Extend directory cluster chain */
    u32 neu = fat_alloc_cluster();
    if (!neu || !prev)
        return -1;
    if (fat_put(prev, neu) != 0)
        return -1;
    *out_lba = clus_to_lba(neu);
    *out_off = 0;
    if (read_sector(*out_lba, sec_out) != 0)
        return -1;
    return 0;
}

/* D7.2: fill_83_dirent — also stamps current RTC time as crt/wrt/acc.
 * date/time left 0 if RTC unavailable. */
static void fill_83_dirent(u8 *e, const char name83[11], u8 attr, u32 clus, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = attr;
    e[20] = (u8)((clus >> 16) & 0xFF);
    e[21] = (u8)((clus >> 24) & 0xFF);
    e[26] = (u8)(clus & 0xFF);
    e[27] = (u8)((clus >> 8) & 0xFF);
    e[28] = (u8)(size & 0xFF);
    e[29] = (u8)((size >> 8) & 0xFF);
    e[30] = (u8)((size >> 16) & 0xFF);
    e[31] = (u8)((size >> 24) & 0xFF);
    extern u64 rtc_unix_seconds(void);
    u64 now = rtc_unix_seconds();
    u16 d = 0, t = 0;
    fat_unix_to_date(now, &d, &t);
    e[14] = (u8)(t & 0xFF);  e[15] = (u8)(t >> 8);   /* crt_time */
    e[16] = (u8)(d & 0xFF);  e[17] = (u8)(d >> 8);   /* crt_date */
    e[18] = (u8)(d & 0xFF);  e[19] = (u8)(d >> 8);   /* acc_date */
    e[22] = (u8)(t & 0xFF);  e[23] = (u8)(t >> 8);   /* wrt_time */
    e[24] = (u8)(d & 0xFF);  e[25] = (u8)(d >> 8);   /* wrt_date */
}

/* Update size/start of a root entry matching 8.3 name. */
static int root_update_dirent(const char name83[11], u32 start_clus, u32 size)
{
    u8 sec[512];
    if (g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return -1;
                if (e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, name83, 11) != 0)
                    continue;
                e[20] = (u8)((start_clus >> 16) & 0xFF);
                e[21] = (u8)((start_clus >> 24) & 0xFF);
                e[26] = (u8)(start_clus & 0xFF);
                e[27] = (u8)((start_clus >> 8) & 0xFF);
                e[28] = (u8)(size & 0xFF);
                e[29] = (u8)((size >> 8) & 0xFF);
                e[30] = (u8)((size >> 16) & 0xFF);
                e[31] = (u8)((size >> 24) & 0xFF);
                return write_sector(g_fat.root_lba + s, sec);
            }
        }
        return -1;
    }

    u32 clus = g_fat.root_clus;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            u32 lba = clus_to_lba(clus) + s;
            if (read_sector(lba, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return -1;
                if (e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, name83, 11) != 0)
                    continue;
                e[20] = (u8)((start_clus >> 16) & 0xFF);
                e[21] = (u8)((start_clus >> 24) & 0xFF);
                e[26] = (u8)(start_clus & 0xFF);
                e[27] = (u8)((start_clus >> 8) & 0xFF);
                e[28] = (u8)(size & 0xFF);
                e[29] = (u8)((size >> 8) & 0xFF);
                e[30] = (u8)((size >> 16) & 0xFF);
                e[31] = (u8)((size >> 24) & 0xFF);
                return write_sector(lba, sec);
            }
        }
        clus = fat_get(clus);
    }
    return -1;
}

/* M21: free the cluster chain starting at `c`. FAT entries → 0.
 * Used by stale-cleanup before re-creating a known file across boots. */
static void fat_free_chain(u32 c)
{
    while (c >= 2 && !fat_is_eof(c)) {
        u32 next = fat_get(c);
        fat_put(c, 0);
        c = next;
    }
}

/* M21: find a root entry by 8.3 name and mark it deleted (0xE5) in place,
 * freeing its data cluster chain. Works for both FAT16 fixed root and
 * FAT32 root cluster chain. Returns 0 if found+freed, -1 otherwise. */
static int root_unlink_and_free(const char name83[11])
{
    u8 sec[512];
    u32 sectors_or_clus;

    if (g_fat.fat_type == 32) {
        sectors_or_clus = g_fat.sec_per_clus;
        u32 clus = g_fat.root_clus;
        while (!fat_is_eof(clus) && clus >= 2) {
            for (u8 s = 0; s < sectors_or_clus; s++) {
                u32 lba = clus_to_lba(clus) + s;
                if (read_sector(lba, sec) != 0)
                    return -1;
                for (u32 i = 0; i < 512; i += 32) {
                    u8 *e = &sec[i];
                    if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                        continue;
                    if (memcmp(e, name83, 11) != 0)
                        continue;
                    u32 c = e[26] | (e[27] << 8);
                    c |= (u32)(e[20] | (e[21] << 8)) << 16;
                    e[0] = 0xE5;
                    if (write_sector(lba, sec) != 0)
                        return -1;
                    if (c >= 2)
                        fat_free_chain(c);
                    return 0;
                }
            }
            clus = fat_get(clus);
        }
        return -1;
    }

    /* FAT16: fixed root region in `root_sectors` sectors starting at root_lba */
    for (u32 s = 0; s < g_fat.root_sectors; s++) {
        if (read_sector(g_fat.root_lba + s, sec) != 0)
            return -1;
        for (u32 i = 0; i < 512; i += 32) {
            u8 *e = &sec[i];
            if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                continue;
            if (memcmp(e, name83, 11) != 0)
                continue;
            u32 c = e[26] | (e[27] << 8);
            e[0] = 0xE5;
            if (write_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            if (c >= 2)
                fat_free_chain(c);
            return 0;
        }
    }
    return -1;
}

/* M24: resolve parent dir + leaf 8.3 name for unlink/rename.
 * Walks path components, last segment is encoded as 8.3 into out_name83.
 * Returns parent cluster (or 0 for FAT16 root region), fills out_name83[11].
 * Returns -1 on any error (not found, mid-component not a dir). */
static int fat_resolve_parent(const char *path, u32 *out_parent, char out_name83[11])
{
    while (*path == '/')
        path++;
    if (!*path)
        return -1; /* root itself not unlinkable */

    u32 dir_clus = (g_fat.fat_type == 32) ? g_fat.root_clus : 0;
    char comp[64];
    for (;;) {
        int n = 0;
        while (*path && *path != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *path++;
        comp[n] = 0;
        while (*path == '/')
            path++;

        char w[11];
        encode_83_upper(comp, w);

        if (!*path) {
            /* leaf component — encode and return */
            memcpy(out_name83, w, 11);
            *out_parent = dir_clus;
            return 0;
        }
        /* mid-component — must be a directory */
        struct fat_dirent_meta m;
        if (find_in_dir(dir_clus, w, &m) != 0)
            return -1;
        if (!(m.attr & 0x10))
            return -1;
        dir_clus = m.clus;
    }
}

/* M24: mark an entry deleted (0xE5) inside a given parent directory cluster chain.
 * Works for both FAT16 root region (parent==0) and FAT32 subdirs.
 * M24.1: free_chain=0 for cross-dir rename (new dirent reuses the same cluster). */
static int dir_unlink_at(u32 parent_clus, const char name83[11], int free_chain)
{
    u8 sec[512];
    if (parent_clus == 0 && g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, name83, 11) != 0)
                    continue;
                u32 c = e[26] | (e[27] << 8);
                e[0] = 0xE5;
                if (write_sector(g_fat.root_lba + s, sec) != 0)
                    return -1;
                if (free_chain && c >= 2)
                    fat_free_chain(c);
                return 0;
            }
        }
        return -1;
    }
    u32 clus = parent_clus ? parent_clus : g_fat.root_clus;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            u32 lba = clus_to_lba(clus) + s;
            if (read_sector(lba, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, name83, 11) != 0)
                    continue;
                u32 c = e[26] | (e[27] << 8);
                if (g_fat.fat_type == 32)
                    c |= (u32)(e[20] | (e[21] << 8)) << 16;
                e[0] = 0xE5;
                if (write_sector(lba, sec) != 0)
                    return -1;
                if (free_chain && c >= 2)
                    fat_free_chain(c);
                return 0;
            }
        }
        clus = fat_get(clus);
    }
    return -1;
}

/* M24: rename a dirent in-place inside its parent (no cross-dir rename).
 * The directory cluster of the entry itself is unchanged. */
static int dir_rename_at(u32 parent_clus, const char old_name83[11],
                         const char new_name83[11])
{
    u8 sec[512];
    int (*visit)(u32, u8 *, int) = 0; /* unused; we duplicate logic for FAT16/FAT32 */
    (void)visit;
    if (parent_clus == 0 && g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, old_name83, 11) != 0)
                    continue;
                memcpy(e, new_name83, 11);
                if (write_sector(g_fat.root_lba + s, sec) != 0)
                    return -1;
                return 0;
            }
        }
        return -1;
    }
    u32 clus = parent_clus ? parent_clus : g_fat.root_clus;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            u32 lba = clus_to_lba(clus) + s;
            if (read_sector(lba, sec) != 0)
                return -1;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
                    continue;
                if (memcmp(e, old_name83, 11) != 0)
                    continue;
                memcpy(e, new_name83, 11);
                if (write_sector(lba, sec) != 0)
                    return -1;
                return 0;
            }
        }
        clus = fat_get(clus);
    }
    return -1;
}

/* M24: is the given directory empty? (Used by rmdir.) */
static int dir_is_empty(u32 dir_clus)
{
    u8 sec[512];
    if (dir_clus == 0 && g_fat.fat_type != 32) {
        for (u32 s = 0; s < g_fat.root_sectors; s++) {
            if (read_sector(g_fat.root_lba + s, sec) != 0)
                return 0;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return 1;
                if (e[0] == 0xE5)
                    continue;
                if (e[11] == 0x0F)
                    continue;
                /* . and .. are always present; skip them */
                if (e[0] == '.' && (e[1] == ' ' || e[1] == '.'))
                    continue;
                return 0; /* some real entry present */
            }
        }
        return 1;
    }
    u32 clus = dir_clus;
    while (!fat_is_eof(clus) && clus >= 2) {
        for (u8 s = 0; s < g_fat.sec_per_clus; s++) {
            u32 lba = clus_to_lba(clus) + s;
            if (read_sector(lba, sec) != 0)
                return 0;
            for (u32 i = 0; i < 512; i += 32) {
                u8 *e = &sec[i];
                if (e[0] == 0)
                    return 1;
                if (e[0] == 0xE5)
                    continue;
                if (e[11] == 0x0F)
                    continue;
                if (e[0] == '.' && (e[1] == ' ' || e[1] == '.'))
                    continue;
                return 0;
            }
        }
        clus = fat_get(clus);
    }
    return 1;
}
/* M24: high-level unlink/rmdir/rename API. Each takes an absolute path
 * resolved against the FAT root. Returns 0 on success, -1 on error. */
int fat_unlink_path(const char *path)
{
    if (!fat_writable_type())
        return -1;
    u32 parent = 0;
    char name83[11];
    if (fat_resolve_parent(path, &parent, name83) != 0)
        return -1;
    return dir_unlink_at(parent, name83, 1);
}

int fat_rmdir_path(const char *path)
{
    if (!fat_writable_type())
        return -1;
    /* Check the leaf is a directory AND empty before unlinking. */
    u32 cl = 0, sz = 0;
    u8 attr = 0;
    if (fat_resolve(path, &cl, &sz, &attr, 0) != 0)
        return -1;
    if (!(attr & 0x10))
        return -1; /* not a directory */
    if (!dir_is_empty(cl))
        return -1;
    u32 parent = 0;
    char name83[11];
    if (fat_resolve_parent(path, &parent, name83) != 0)
        return -1;
    return dir_unlink_at(parent, name83, 1);
}

/* M24.1: update the `..` entry inside a moved directory to point at its new
 * parent. dir_clus is the moved dir's first cluster; new_parent is the target
 * parent cluster (0 = FAT16 root region). */
static int fat_update_dotdot(u32 dir_clus, u32 new_parent)
{
    u8 sec[512];
    if (read_sector(clus_to_lba(dir_clus), sec) != 0)
        return -1;
    u8 *e = &sec[32]; /* second dirent is `..` (first is `.`) */
    if (!(e[0] == '.' && e[1] == '.'))
        return -1; /* unexpected layout — refuse to corrupt */
    u32 p = (new_parent == 0 && g_fat.fat_type != 32) ? 0 : new_parent;
    e[20] = (u8)((p >> 16) & 0xFF);
    e[21] = (u8)((p >> 24) & 0xFF);
    e[26] = (u8)(p & 0xFF);
    e[27] = (u8)((p >> 8) & 0xFF);
    return write_sector(clus_to_lba(dir_clus), sec);
}

/* M24.1: cross-directory rename — write a new dirent in the target parent that
 * reuses the source cluster chain, then delete the source dirent WITHOUT
 * freeing the chain. Preserves the source timestamps. */
static int dir_rename_cross(u32 old_parent, u32 new_parent,
                            const char old_name83[11], const char new_name83[11])
{
    struct fat_dirent_meta m;
    if (find_in_dir(old_parent, old_name83, &m) != 0)
        return -1;
    /* refuse overwrite of an existing target */
    if (find_in_dir(new_parent, new_name83, 0) == 0)
        return -1;

    u8 sec[512];
    u64 lba;
    u32 off;
    if (dir_find_free_slot(new_parent, &lba, &off, sec) != 0)
        return -1;
    u8 *e = &sec[off];
    fill_83_dirent(e, new_name83, m.attr, m.clus, m.size);
    /* fill_83_dirent stamps current RTC; preserve the source's timestamps */
    e[14] = (u8)(m.crt_time & 0xFF);  e[15] = (u8)(m.crt_time >> 8);
    e[16] = (u8)(m.crt_date & 0xFF);  e[17] = (u8)(m.crt_date >> 8);
    e[18] = (u8)(m.acc_date & 0xFF);  e[19] = (u8)(m.acc_date >> 8);
    e[22] = (u8)(m.wrt_time & 0xFF);  e[23] = (u8)(m.wrt_time >> 8);
    e[24] = (u8)(m.wrt_date & 0xFF);  e[25] = (u8)(m.wrt_date >> 8);
    if (write_sector(lba, sec) != 0)
        return -1;

    if ((m.attr & 0x10) && fat_update_dotdot(m.clus, new_parent) != 0)
        return -1;
    return dir_unlink_at(old_parent, old_name83, 0);
}

/* M24.1: walk a directory's parent chain via its `..` entry to see if
 * target_clus is an ancestor. Used to refuse moving a directory into its own
 * subtree (would create a cycle). Handles FAT32 root's self-referential `..`. */
static int fat_dir_has_ancestor(u32 dir_clus, u32 target_clus)
{
    u8 sec[512];
    u32 c = dir_clus;
    while (c >= 2) {
        if (read_sector(clus_to_lba(c), sec) != 0)
            return 0;
        u8 *e = &sec[32]; /* `..` */
        if (!(e[0] == '.' && e[1] == '.'))
            return 0;
        u32 parent = e[26] | (e[27] << 8);
        if (g_fat.fat_type == 32)
            parent |= (u32)(e[20] | (e[21] << 8)) << 16;
        if (parent == target_clus)
            return 1;
        if (parent == 0 || parent == c)
            break; /* reached root (or root's self-loop) */
        c = parent;
    }
    return 0;
}

int fat_rename_path(const char *oldp, const char *newp)
{
    if (!fat_writable_type())
        return -1;
    u32 old_parent = 0, new_parent = 0;
    char old83[11], new83[11];
    if (fat_resolve_parent(oldp, &old_parent, old83) != 0)
        return -1;
    if (fat_resolve_parent(newp, &new_parent, new83) != 0)
        return -1;
    if (old_parent == new_parent)
        return dir_rename_at(old_parent, old83, new83);

    /* cross-directory: refuse moving a directory into itself or a descendant */
    struct fat_dirent_meta m;
    if (find_in_dir(old_parent, old83, &m) != 0)
        return -1;
    if ((m.attr & 0x10) && (new_parent == m.clus ||
                            fat_dir_has_ancestor(new_parent, m.clus)))
        return -1;
    return dir_rename_cross(old_parent, new_parent, old83, new83);
}

static int fat_create_root(const char *path, int is_dir, u32 *out_clus)
{
    if (!fat_writable_type())
        return -1;
    while (*path == '/')
        path++;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            return -1; /* root only */
    }
    char w[11];
    encode_83_upper(path, w);
    struct fat_dirent_meta m0;
    u32 root_key = (g_fat.fat_type == 32) ? g_fat.root_clus : 0;
    if (find_in_dir(root_key, w, &m0) == 0)
        return -1; /* EEXIST */

    u32 cl = fat_alloc_cluster();
    if (!cl)
        return -1;
    if (is_dir) {
        u8 dirsec[512];
        memset(dirsec, 0, sizeof(dirsec));
        char dot[11];
        memset(dot, ' ', 11);
        dot[0] = '.';
        fill_83_dirent(dirsec, dot, 0x10, cl, 0);
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[1] = '.';
        fill_83_dirent(dirsec + 32, dot, 0x10,
                       g_fat.fat_type == 32 ? g_fat.root_clus : 0, 0);
        if (write_sectors(clus_to_lba(cl), 1, dirsec) != 0)
            return -1;
        /* remaining sectors of cluster already zeroed by alloc */
    }

    u8 sec[512];
    u64 lba;
    u32 off;
    if (dir_find_free_slot(root_key, &lba, &off, sec) != 0)
        return -1;
    fill_83_dirent(sec + off, w, is_dir ? 0x10 : 0x20, cl, 0);
    if (write_sector(lba, sec) != 0)
        return -1;
    if (out_clus)
        *out_clus = cl;
    return 0;
}

static int fat_write_file(struct vfs_file *f, const void *buf, u64 len, u64 *out_n)
{
    struct fat_file *ff = f->fs_priv;
    if (!ff || !f->writable || !fat_writable_type())
        return -1;
    if (len == 0) {
        *out_n = 0;
        return 0;
    }

    u32 clus_size = (u32)g_fat.sec_per_clus * g_fat.byts_per_sec;
    if (ff->start_clus < 2) {
        u32 cl = fat_alloc_cluster();
        if (!cl)
            return -1;
        ff->start_clus = cl;
    }

    const u8 *src = buf;
    u64 remain = len;
    u64 pos = ff->pos;
    u32 clus = ff->start_clus;
    u64 skip = pos / clus_size;
    while (skip--) {
        u32 n = fat_get(clus);
        if (fat_is_eof(n) || n < 2) {
            n = fat_alloc_cluster();
            if (!n)
                return -1;
            if (fat_put(clus, n) != 0)
                return -1;
            clus = n;
        } else {
            clus = n;
        }
    }
    u32 off_in = (u32)(pos % clus_size);
    u8 *secbuf = kmalloc(clus_size);
    if (!secbuf)
        return -1;

    while (remain) {
        if (read_sectors(clus_to_lba(clus), g_fat.sec_per_clus, secbuf) != 0) {
            kfree(secbuf);
            return -1;
        }
        u32 chunk = clus_size - off_in;
        if (chunk > remain)
            chunk = (u32)remain;
        memcpy(secbuf + off_in, src, chunk);
        if (write_sectors(clus_to_lba(clus), g_fat.sec_per_clus, secbuf) != 0) {
            kfree(secbuf);
            return -1;
        }
        src += chunk;
        remain -= chunk;
        pos += chunk;
        off_in = 0;
        if (remain) {
            u32 n = fat_get(clus);
            if (fat_is_eof(n) || n < 2) {
                n = fat_alloc_cluster();
                if (!n) {
                    kfree(secbuf);
                    return -1;
                }
                if (fat_put(clus, n) != 0) {
                    kfree(secbuf);
                    return -1;
                }
                clus = n;
            } else {
                clus = n;
            }
        }
    }
    kfree(secbuf);
    ff->pos = pos;
    if (pos > ff->size)
        ff->size = pos;
    f->pos = ff->pos;
    f->size = ff->size;

    if (root_update_dirent(ff->name83, ff->start_clus, (u32)ff->size) != 0)
        kprintf("[fat] warning: dirent update failed\n");
    *out_n = len;
    return 0;
}

static int fat_mkdir_rw(const char *path, int mode)
{
    (void)mode;
    if (!fat_writable_type())
        return -1;
    return fat_create_root(path, 1, 0);
}

static int fat_open(const char *path, int flags, struct vfs_file **out)
{
    const char *p = path;
    while (*p == '/')
        p++;
    if (!*p) {
        struct vfs_file *vf = kmalloc(sizeof(*vf));
        if (!vf)
            return -1;
        memset(vf, 0, sizeof(*vf));
        vf->ops = fat_vfs_ops();
        vf->is_dir = 1;
        vf->pos = 0;
        *out = vf;
        return 0;
    }

    u32 cl = 0, sz = 0;
    u8 attr = 0;
    struct fat_dirent_meta meta;
    int need_create = (flags & VFS_O_CREAT) != 0;
    if (fat_resolve(path, &cl, &sz, &attr, &meta) != 0) {
        if (!need_create || !fat_writable_type())
            return -1;
        if (fat_create_root(path, 0, &cl) != 0)
            return -1;
        sz = 0;
        memset(&meta, 0, sizeof(meta));
        meta.clus = cl;
        /* Stamp creation time on newly created file (D7.2). */
        extern u64 rtc_unix_seconds(void);
        u64 now = rtc_unix_seconds();
        fat_unix_to_date(now, &meta.crt_date, &meta.crt_time);
        meta.wrt_date = meta.crt_date;
        meta.wrt_time = meta.crt_time;
        meta.acc_date = meta.crt_date;
    } else if (attr & 0x10) {
        /* M20: leaf is a directory — open for getdents64 (no read/write). */
        struct fat_dir_iter *it = kmalloc(sizeof(*it));
        if (!it)
            return -1;
        memset(it, 0, sizeof(*it));
        /* FAT16 root uses cluster 0 + root_lba/root_sectors; otherwise store
         * the cluster and let getdents64 walk the chain. */
        it->clus = (g_fat.fat_type == 32) ? (u16)cl : 0;
        struct vfs_file *vf = kmalloc(sizeof(*vf));
        if (!vf) {
            kfree(it);
            return -1;
        }
        memset(vf, 0, sizeof(*vf));
        vf->ops = fat_vfs_ops();
        vf->fs_priv = it;
        vf->is_dir = 1;
        vf->pos = 0;
        vf->size = 0;
        *out = vf;
        return 0;
    } else if ((flags & VFS_O_TRUNC) && fat_writable_type()) {
        /* Truncate: mark first cluster EOF, size 0 (may leak old chain). */
        if (cl >= 2)
            fat_put(cl, fat_eof_mark());
        sz = 0;
    }

    struct fat_file *ff = kmalloc(sizeof(*ff));
    if (!ff)
        return -1;
    memset(ff, 0, sizeof(*ff));
    encode_83_upper(path, ff->name83);
    /* path may be /HELIXW.TXT — encode_83_upper uses whole string; strip dirs */
    {
        const char *leaf = path;
        for (const char *q = path; *q; q++)
            if (*q == '/')
                leaf = q + 1;
        encode_83_upper(leaf, ff->name83);
    }
    ff->start_clus = cl;
    ff->size = sz;
    ff->pos = (flags & VFS_O_APPEND) ? sz : 0;
    /* D7.2: capture dirent date/time for fstat */
    ff->crt_time = meta.crt_time;
    ff->crt_date = meta.crt_date;
    ff->acc_date = meta.acc_date;
    ff->wrt_time = meta.wrt_time;
    ff->wrt_date = meta.wrt_date;
    struct vfs_file *vf = kmalloc(sizeof(*vf));
    if (!vf) {
        kfree(ff);
        return -1;
    }
    memset(vf, 0, sizeof(*vf));
    vf->ops = fat_vfs_ops();
    vf->fs_priv = ff;
    vf->size = sz;
    vf->pos = ff->pos;
    vf->writable = (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) ? 1 : 0;
    *out = vf;
    return 0;
}

static const struct vfs_ops g_fat_ops = {
    .open = fat_open,
    .read = fat_read_file,
    .write = fat_write_file,
    .close = fat_close,
    .readdir_root = fat_readdir_root,
    .getdents64 = fat_getdents64,
    .fstat = fat_fstat,
    .mkdir = fat_mkdir_rw,
};

const struct vfs_ops *fat_vfs_ops(void)
{
    return &g_fat_ops;
}

int fat_is_mounted(void)
{
    return g_fat.ready;
}

int fat_mount(u64 part_lba, u64 part_sectors)
{
    memset(&g_fat, 0, sizeof(g_fat));
    g_fat.part_lba = part_lba;
    g_fat.part_sectors = part_sectors;

    u8 bpb[512];
    if (blk_read(part_lba, 1, bpb) != 0)
        return -1;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        kprintf("[fat] bad signature\n");
        return -1;
    }

    g_fat.byts_per_sec = bpb[11] | (bpb[12] << 8);
    g_fat.sec_per_clus = bpb[13];
    g_fat.rsvd_sec_cnt = bpb[14] | (bpb[15] << 8);
    g_fat.num_fats = bpb[16];
    g_fat.root_ent_cnt = bpb[17] | (bpb[18] << 8);
    u16 tot16 = bpb[19] | (bpb[20] << 8);
    u16 fatsz16 = bpb[22] | (bpb[23] << 8);
    u32 tot32 = bpb[32] | (bpb[33] << 8) | (bpb[34] << 16) | (bpb[35] << 24);
    u32 fatsz32 = bpb[36] | (bpb[37] << 8) | (bpb[38] << 16) | (bpb[39] << 24);

    g_fat.fatsz = fatsz16 ? fatsz16 : fatsz32;
    g_fat.totsec = tot16 ? tot16 : tot32;
    if (!g_fat.byts_per_sec || !g_fat.sec_per_clus || !g_fat.fatsz) {
        kprintf("[fat] invalid BPB\n");
        return -1;
    }

    g_fat.root_sectors = ((g_fat.root_ent_cnt * 32) + (g_fat.byts_per_sec - 1)) / g_fat.byts_per_sec;
    g_fat.first_fat_lba = g_fat.rsvd_sec_cnt;
    g_fat.root_lba = g_fat.rsvd_sec_cnt + g_fat.num_fats * g_fat.fatsz;
    g_fat.data_lba = g_fat.root_lba + g_fat.root_sectors;

    u32 data_sec = g_fat.totsec - g_fat.data_lba;
    g_fat.data_clusters = data_sec / g_fat.sec_per_clus;

    if (g_fat.root_ent_cnt == 0) {
        g_fat.fat_type = 32;
        g_fat.root_clus = bpb[44] | (bpb[45] << 8) | (bpb[46] << 16) | (bpb[47] << 24);
    } else if (g_fat.data_clusters < 4085)
        g_fat.fat_type = 12;
    else
        g_fat.fat_type = 16;

    g_fat.ready = 1;
    kprintf("[fat] mounted FAT%u spc=%u fatsz=%u root_ent=%u data_lba=%u clusters=%u\n",
            g_fat.fat_type, g_fat.sec_per_clus, g_fat.fatsz, g_fat.root_ent_cnt,
            g_fat.data_lba, g_fat.data_clusters);
    if (g_fat.fat_type == 16 || g_fat.fat_type == 32)
        kprintf("[fat] write/mkdir enabled on root (FAT%u)\n", g_fat.fat_type);
    else
        kprintf("[fat] write/mkdir disabled (FAT%u)\n", g_fat.fat_type);
    return 0;
}

int fat_selftest_write(void)
{
    if (!g_fat.ready || !fat_writable_type()) {
        kprintf("[fat] selftest skip (need FAT16/32)\n");
        return -1;
    }

    const char *path = "/HELIXW.TXT";
    const char *payload = "HelixFATWriteOK\n";
    u64 plen = 0;
    while (payload[plen])
        plen++;

    /* Delete stale entry if present so create is clean across boots on same img.
     * M21: works for both FAT16 and FAT32 (was FAT16-only, caused flaky re-runs). */
    {
        char w[11];
        encode_83_upper("HELIXW.TXT", w);
        root_unlink_and_free(w);
    }

    struct vfs_file *f = 0;
    if (fat_open(path, VFS_O_CREAT | VFS_O_TRUNC | VFS_O_WRONLY, &f) != 0) {
        kprintf("[fat] selftest open-create failed\n");
        return -1;
    }
    u64 nw = 0;
    if (fat_write_file(f, payload, plen, &nw) != 0 || nw != plen) {
        kprintf("[fat] selftest write failed nw=%llu\n", (unsigned long long)nw);
        fat_close(f);
        return -1;
    }
    fat_close(f);

    /* Resolve via path lookup (proves dirent visible) */
    u32 cl = 0, sz = 0;
    if (fat_resolve(path, &cl, &sz, 0, 0) != 0) {
        kprintf("[fat] selftest resolve after write failed\n");
        return -1;
    }
    kprintf("[fat] selftest resolved clus=%u size=%u\n", cl, sz);

    f = 0;
    if (fat_open(path, VFS_O_RDONLY, &f) != 0) {
        kprintf("[fat] selftest reopen failed\n");
        return -1;
    }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    u64 nr = 0;
    if (fat_read_file(f, buf, sizeof(buf) - 1, &nr) != 0 || nr != plen) {
        kprintf("[fat] selftest readback size mismatch nr=%llu expect=%llu\n",
                (unsigned long long)nr, (unsigned long long)plen);
        fat_close(f);
        return -1;
    }
    fat_close(f);
    if (memcmp(buf, payload, (size_t)plen) != 0) {
        kprintf("[fat] selftest content mismatch\n");
        return -1;
    }
    kprintf("[fat] selftest OK (FAT%u): %s", g_fat.fat_type, payload);
    kprintf("[fs] HelixFATWriteOK\n");
    return 0;
}
