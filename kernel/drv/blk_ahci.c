#include "helix/blk.h"
#include "helix/cpuio.h"
#include "helix/kprintf.h"
#include "helix/pmm.h"
#include "helix/paging.h"
#include "helix/string.h"
#include "helix/panic.h"

/* Minimal AHCI (SATA) read for QEMU ich9-ahci + ide-hd on port 0.
 * PCI: bus scan for class 0x0106, BAR5 = ABAR (HBA MMIO).
 */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct blk_dev g_blk;

static u32 pci_read32(u8 bus, u8 dev, u8 func, u8 offset)
{
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(u8 bus, u8 dev, u8 func, u8 offset, u32 val)
{
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static u16 pci_read16(u8 bus, u8 dev, u8 func, u8 offset)
{
    u32 v = pci_read32(bus, dev, func, offset & 0xFC);
    return (u16)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

static volatile u32 *mm_u32(u64 base, u32 off)
{
    return (volatile u32 *)(uintptr_t)(base + off);
}

static int ahci_find(u8 *bus, u8 *dev, u8 *func, u64 *abar_out)
{
    for (u8 b = 0; b < 16; b++) {
        for (u8 d = 0; d < 32; d++) {
            for (u8 f = 0; f < 8; f++) {
                u16 vendor = pci_read16(b, d, f, 0x00);
                if (vendor == 0xFFFF)
                    continue;
                u32 classreg = pci_read32(b, d, f, 0x08);
                u8 baseclass = (u8)((classreg >> 24) & 0xFF);
                u8 subclass  = (u8)((classreg >> 16) & 0xFF);
                if (baseclass == 0x01 && subclass == 0x06) {
                    u32 bar5 = pci_read32(b, d, f, 0x24);
                    if (bar5 == 0 || bar5 == 0xFFFFFFFF)
                        continue;
                    u64 abar = bar5 & ~0xFull;
                    /* 64-bit memory BAR? type bits [2:1] == 10b */
                    if ((bar5 & 0x6) == 0x4) {
                        u32 bar6 = pci_read32(b, d, f, 0x28);
                        abar |= (u64)bar6 << 32;
                    }
                    /* Enable bus master + MMIO */
                    u32 cmd = pci_read32(b, d, f, 0x04);
                    cmd |= 0x6;
                    pci_write32(b, d, f, 0x04, cmd);
                    *bus = b;
                    *dev = d;
                    *func = f;
                    *abar_out = abar;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* HBA / port offsets */
#define HBA_GHC      0x04
#define HBA_PI       0x0C
#define HBA_PORT0    0x100
#define PORT_CLB     0x00
#define PORT_CLBU    0x04
#define PORT_FB      0x08
#define PORT_FBU     0x0C
#define PORT_IS      0x10
#define PORT_IE      0x14
#define PORT_CMD     0x18
#define PORT_TFD     0x20
#define PORT_SIG     0x24
#define PORT_SSTS    0x28
#define PORT_SCTL    0x2C
#define PORT_SERR    0x30
#define PORT_CI      0x38

#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)

#define TFD_BSY (1u << 7)
#define TFD_DRQ (1u << 3)

#define SATA_SIG_ATA 0x00000101u

struct hba_cmd_header {
    u16 flags;      /* bits: CFL,A,W,P,R,B,C,PMP + PRDTL in high */
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 reserved[4];
} __attribute__((packed));

struct hba_prdt_entry {
    u32 dba;
    u32 dbau;
    u32 reserved;
    u32 dbc; /* bit31=I, low 22 = byte count - 1 */
} __attribute__((packed));

struct hba_cmd_table {
    u8  cfis[64];
    u8  acmd[16];
    u8  reserved[48];
    struct hba_prdt_entry prdt[1];
} __attribute__((packed));

static void port_stop(u64 port)
{
    u32 cmd = *mm_u32(port, PORT_CMD);
    cmd &= ~PORT_CMD_ST;
    cmd &= ~PORT_CMD_FRE;
    *mm_u32(port, PORT_CMD) = cmd;
    for (int i = 0; i < 100000; i++) {
        cmd = *mm_u32(port, PORT_CMD);
        if (!(cmd & (PORT_CMD_CR | PORT_CMD_FR)))
            break;
    }
}

static void port_start(u64 port)
{
    u32 cmd;
    for (int i = 0; i < 100000; i++) {
        cmd = *mm_u32(port, PORT_CMD);
        if (!(cmd & PORT_CMD_CR))
            break;
    }
    cmd = *mm_u32(port, PORT_CMD);
    cmd |= PORT_CMD_FRE;
    cmd |= PORT_CMD_ST;
    *mm_u32(port, PORT_CMD) = cmd;
}

int blk_init(void)
{
    memset(&g_blk, 0, sizeof(g_blk));

    u8 bus, dev, func;
    u64 abar;
    if (ahci_find(&bus, &dev, &func, &abar) != 0) {
        kprintf("[blk] no AHCI controller on PCI\n");
        return -1;
    }
    kprintf("[blk] AHCI at %x:%x.%u ABAR=0x%llx\n",
            (unsigned)bus, (unsigned)dev, (unsigned)func,
            (unsigned long long)abar);

    /* ABAR is usually high MMIO — outside low identity map. */
    if (paging_map_mmio(abar, 0x1100) != 0) {
        kprintf("[blk] map AHCI MMIO failed\n");
        return -1;
    }

    g_blk.abar = abar;
    *mm_u32(abar, HBA_GHC) |= (1u << 31);

    u32 pi = *mm_u32(abar, HBA_PI);
    if (!(pi & 1u)) {
        kprintf("[blk] port0 not implemented (PI=0x%x)\n", pi);
        return -1;
    }
    u64 port = abar + HBA_PORT0;
    g_blk.port_mm = port;

    u32 ssts = *mm_u32(port, PORT_SSTS);
    u32 det = ssts & 0xF;
    u32 ipm = (ssts >> 8) & 0xF;
    kprintf("[blk] port0 SSTS=0x%x det=%u ipm=%u sig=0x%x\n",
            ssts, det, ipm, *mm_u32(port, PORT_SIG));
    if (det != 3 || ipm != 1) {
        kprintf("[blk] port0 device not ready\n");
        return -1;
    }

    port_stop(port);

    /* Allocate command list (1024), FIS (256), cmd table (256+) from PMM */
    u64 cl = pmm_alloc_page();
    u64 fb = pmm_alloc_page();
    u64 ct = pmm_alloc_page();
    u64 db = pmm_alloc_page(); /* data bounce for DMA */
    if (!cl || !fb || !ct || !db)
        return -1;
    memset((void *)(uintptr_t)cl, 0, PAGE_SIZE);
    memset((void *)(uintptr_t)fb, 0, PAGE_SIZE);
    memset((void *)(uintptr_t)ct, 0, PAGE_SIZE);
    memset((void *)(uintptr_t)db, 0, PAGE_SIZE);
    g_blk.cmd_list = (void *)(uintptr_t)cl;
    g_blk.fis_base = (void *)(uintptr_t)fb;
    g_blk.cmd_table = (void *)(uintptr_t)ct;
    g_blk.dma_buf = (void *)(uintptr_t)db;

    *mm_u32(port, PORT_CLB)  = (u32)cl;
    *mm_u32(port, PORT_CLBU) = 0;
    *mm_u32(port, PORT_FB)   = (u32)fb;
    *mm_u32(port, PORT_FBU)  = 0;

    /* clear IS / SERR */
    *mm_u32(port, PORT_IS) = 0xFFFFFFFF;
    *mm_u32(port, PORT_SERR) = 0xFFFFFFFF;

    port_start(port);

    g_blk.ready = 1;
    kprintf("[blk] AHCI port0 ready\n");
    return 0;
}

int blk_ready(void)
{
    return g_blk.ready;
}

/* is_write: 0=READ DMA EXT (0x25), 1=WRITE DMA EXT (0x35) */
static int blk_rw(u64 lba, u32 count, void *buf, int is_write)
{
    if (!g_blk.ready || !buf || count == 0)
        return -1;
    if (count > 8) {
        u8 *p = buf;
        while (count) {
            u32 n = count > 8 ? 8 : count;
            if (blk_rw(lba, n, p, is_write) != 0)
                return -1;
            lba += n;
            p += n * BLK_SECTOR_SIZE;
            count -= n;
        }
        return 0;
    }

    u64 port = g_blk.port_mm;
    for (int i = 0; i < 1000000; i++) {
        u32 tfd = *mm_u32(port, PORT_TFD);
        if (!(tfd & (TFD_BSY | TFD_DRQ)))
            break;
    }

    struct hba_cmd_header *hdr = (struct hba_cmd_header *)g_blk.cmd_list;
    memset(hdr, 0, sizeof(*hdr));
    /* CFL=5; W bit (bit6 of flags low byte area via bit 6 of flags field):
     * flags layout: CFL:5, A:1, W:1, P:1 in low 8 bits of flags. */
    hdr->flags = (u16)(5 | (is_write ? (1u << 6) : 0));
    hdr->prdtl = 1;
    hdr->ctba  = (u32)(uintptr_t)g_blk.cmd_table;
    hdr->ctbau = 0;

    struct hba_cmd_table *tbl = (struct hba_cmd_table *)g_blk.cmd_table;
    memset(tbl, 0, sizeof(*tbl));

    u8 *cfis = tbl->cfis;
    cfis[0] = 0x27;
    cfis[1] = 1 << 7;
    cfis[2] = is_write ? 0x35 : 0x25; /* WRITE/READ DMA EXT */
    cfis[4] = (u8)(lba & 0xFF);
    cfis[5] = (u8)((lba >> 8) & 0xFF);
    cfis[6] = (u8)((lba >> 16) & 0xFF);
    cfis[7] = 1 << 6;
    cfis[8] = (u8)((lba >> 24) & 0xFF);
    cfis[9] = (u8)((lba >> 32) & 0xFF);
    cfis[10] = (u8)((lba >> 40) & 0xFF);
    cfis[12] = (u8)(count & 0xFF);
    cfis[13] = (u8)((count >> 8) & 0xFF);

    /* Always DMA through identity-mapped bounce (caller buf may be stack/heap). */
    u32 nbytes = count * BLK_SECTOR_SIZE;
    if (nbytes > 4096)
        return -1;
    if (is_write)
        memcpy(g_blk.dma_buf, buf, nbytes);

    tbl->prdt[0].dba  = (u32)(uintptr_t)g_blk.dma_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = nbytes - 1;
    tbl->prdt[0].dbc |= (1u << 31);

    *mm_u32(port, PORT_IS) = 0xFFFFFFFF;
    *mm_u32(port, PORT_CI) = 1;

    for (int i = 0; i < 2000000; i++) {
        if (!(*mm_u32(port, PORT_CI) & 1u))
            break;
        if (*mm_u32(port, PORT_IS) & (1u << 30)) {
            kprintf("[blk] %s TFES lba=%llu\n",
                    is_write ? "write" : "read", (unsigned long long)lba);
            return -1;
        }
    }
    if (*mm_u32(port, PORT_CI) & 1u) {
        kprintf("[blk] %s timeout lba=%llu\n",
                is_write ? "write" : "read", (unsigned long long)lba);
        return -1;
    }
    if (!is_write)
        memcpy(buf, g_blk.dma_buf, nbytes);
    return 0;
}

int blk_read(u64 lba, u32 count, void *buf)
{
    return blk_rw(lba, count, buf, 0);
}

int blk_write(u64 lba, u32 count, const void *buf)
{
    return blk_rw(lba, count, (void *)buf, 1);
}

void *blk_dma_buf(void)
{
    return g_blk.dma_buf;
}
