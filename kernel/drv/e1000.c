/*
 * Minimal Intel 82540EM (e1000) NIC driver for QEMU.
 * Polling TX/RX only — enough for ARP + ICMP self-test.
 */
#include "helix/net.h"
#include "helix/pci.h"
#include "helix/paging.h"
#include "helix/pmm.h"
#include "helix/kprintf.h"
#include "helix/string.h"

#define E1000_VENDOR 0x8086u
#define E1000_DEVID  0x100Eu  /* 82540EM, QEMU default */

/* MMIO registers */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EECD     0x0010
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL      0x5400
#define REG_RAH      0x5404

#define CTRL_FD      (1u << 0)
#define CTRL_ASDE    (1u << 5)
#define CTRL_SLU     (1u << 6)
#define CTRL_RST     (1u << 26)

#define RCTL_EN      (1u << 1)
#define RCTL_SBP     (1u << 2)
#define RCTL_UPE     (1u << 3)
#define RCTL_MPE     (1u << 4)
#define RCTL_LPE     (1u << 5)
#define RCTL_LBM_NONE (0u << 6)
#define RCTL_BAM     (1u << 15)
#define RCTL_BSIZE_2048 (0u << 16)
#define RCTL_SECRC   (1u << 26)

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)
#define TCTL_CT_SHIFT  4
#define TCTL_COLD_SHIFT 12

#define RX_DESC_N 32
#define TX_DESC_N 32
#define RX_BUF_SZ 2048
#define TX_BUF_SZ 2048

struct e1000_rx_desc {
    u64 addr;
    u16 length;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed));

#define RDESC_DD   0x01
#define RDESC_EOP  0x02
#define TDESC_EOP  0x01
#define TDESC_IFCS 0x02
#define TDESC_RS   0x08
#define TDESC_DD   0x01

struct e1000 {
    int ready;
    volatile u32 *mmio;
    u64 mmio_phys;
    u8  mac[6];
    struct e1000_rx_desc *rxd;
    struct e1000_tx_desc *txd;
    u64 rxd_phys, txd_phys;
    u8 *rx_buf;
    u8 *tx_buf;
    u64 rx_buf_phys, tx_buf_phys;
    u16 rx_tail;
    u16 tx_tail;
    u16 tx_head_cache;
};

static struct e1000 g_e;

static inline void ew(u32 reg, u32 val)
{
    g_e.mmio[reg / 4] = val;
}
static inline u32 er(u32 reg)
{
    return g_e.mmio[reg / 4];
}

static void eeprom_read_mac(void)
{
    /* QEMU e1000 often has MAC in RAL/RAH already after reset;
     * also try EERD for EEPROM words 0..2. */
    for (int i = 0; i < 3; i++) {
        ew(REG_EERD, (u32)((i << 8) | 1u)); /* addr + start */
        for (int t = 0; t < 100000; t++) {
            u32 v = er(REG_EERD);
            if (v & (1u << 4)) { /* DONE */
                u16 w = (u16)(v >> 16);
                g_e.mac[i * 2]     = (u8)(w & 0xFF);
                g_e.mac[i * 2 + 1] = (u8)(w >> 8);
                break;
            }
        }
    }
    /* Fallback: read RAL/RAH */
    if (g_e.mac[0] == 0 && g_e.mac[1] == 0) {
        u32 ral = er(REG_RAL);
        u32 rah = er(REG_RAH);
        g_e.mac[0] = (u8)(ral);
        g_e.mac[1] = (u8)(ral >> 8);
        g_e.mac[2] = (u8)(ral >> 16);
        g_e.mac[3] = (u8)(ral >> 24);
        g_e.mac[4] = (u8)(rah);
        g_e.mac[5] = (u8)(rah >> 8);
    }
    if (g_e.mac[0] == 0 && g_e.mac[1] == 0) {
        g_e.mac[0] = 0x52; g_e.mac[1] = 0x54; g_e.mac[2] = 0x00;
        g_e.mac[3] = 0x12; g_e.mac[4] = 0x34; g_e.mac[5] = 0x56;
    }
}

static int find_e1000(u8 *bus, u8 *dev, u8 *func)
{
    for (u8 b = 0; b < 32; b++) {
        for (u8 d = 0; d < 32; d++) {
            for (u8 f = 0; f < 8; f++) {
                if (pci_read16(b, d, f, 0x00) != E1000_VENDOR)
                    continue;
                u16 did = pci_read16(b, d, f, 0x02);
                /* QEMU: 100E (82540EM), 10D3 (82574L), 100F, 1209... */
                if (did == 0x100E || did == 0x100F || did == 0x10D3 ||
                    did == 0x1209 || did == 0x1026) {
                    *bus = b; *dev = d; *func = f;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int e1000_init(void)
{
    memset(&g_e, 0, sizeof(g_e));

    u8 bus, dev, func;
    if (find_e1000(&bus, &dev, &func) != 0) {
        kprintf("[net] e1000 not found on PCI\n");
        return -1;
    }
    kprintf("[net] e1000 at %x:%x.%u\n",
            (unsigned)bus, (unsigned)dev, (unsigned)func);

    pci_enable_bus_master(bus, dev, func);

    u64 bar0 = pci_read_bar(bus, dev, func, 0x10, 0);
    if (!bar0) {
        kprintf("[net] e1000 BAR0 missing\n");
        return -1;
    }
    /* Map 128 KiB MMIO window */
    if (paging_map_mmio(bar0, 0x20000) != 0) {
        kprintf("[net] e1000 MMIO map failed\n");
        return -1;
    }
    g_e.mmio_phys = bar0;
    g_e.mmio = (volatile u32 *)(uintptr_t)bar0;
    kprintf("[net] e1000 mmio=0x%llx status=0x%x\n",
            (unsigned long long)bar0, er(REG_STATUS));

    /* Disable interrupts */
    ew(REG_IMC, 0xFFFFFFFF);
    er(REG_ICR);

    /* Reset */
    ew(REG_CTRL, er(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++)
        ;
    ew(REG_IMC, 0xFFFFFFFF);

    /* Link up, auto-speed, full duplex */
    ew(REG_CTRL, er(REG_CTRL) | CTRL_SLU | CTRL_ASDE | CTRL_FD);

    eeprom_read_mac();
    kprintf("[net] MAC %x:%x:%x:%x:%x:%x\n",
            g_e.mac[0], g_e.mac[1], g_e.mac[2],
            g_e.mac[3], g_e.mac[4], g_e.mac[5]);

    /* Program MAC into RAL0/RAH0 */
    u32 ral = (u32)g_e.mac[0] | ((u32)g_e.mac[1] << 8) |
              ((u32)g_e.mac[2] << 16) | ((u32)g_e.mac[3] << 24);
    u32 rah = (u32)g_e.mac[4] | ((u32)g_e.mac[5] << 8) | (1u << 31); /* AV */
    ew(REG_RAL, ral);
    ew(REG_RAH, rah);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        ew(REG_MTA + (u32)i * 4, 0);

    /* Alloc rings + buffers (contiguous pages, identity-mapped low phys) */
    u64 rxd_pages = (sizeof(struct e1000_rx_desc) * RX_DESC_N + 4095) / 4096;
    u64 txd_pages = (sizeof(struct e1000_tx_desc) * TX_DESC_N + 4095) / 4096;
    u64 rxb_pages = (RX_DESC_N * RX_BUF_SZ + 4095) / 4096;
    u64 txb_pages = (TX_DESC_N * TX_BUF_SZ + 4095) / 4096;

    g_e.rxd_phys = pmm_alloc_pages(rxd_pages ? rxd_pages : 1);
    g_e.txd_phys = pmm_alloc_pages(txd_pages ? txd_pages : 1);
    g_e.rx_buf_phys = pmm_alloc_pages(rxb_pages);
    g_e.tx_buf_phys = pmm_alloc_pages(txb_pages);
    if (!g_e.rxd_phys || !g_e.txd_phys || !g_e.rx_buf_phys || !g_e.tx_buf_phys) {
        kprintf("[net] e1000 ring alloc failed\n");
        return -1;
    }
    memset((void *)(uintptr_t)g_e.rxd_phys, 0, rxd_pages * 4096);
    memset((void *)(uintptr_t)g_e.txd_phys, 0, txd_pages * 4096);
    memset((void *)(uintptr_t)g_e.rx_buf_phys, 0, rxb_pages * 4096);
    memset((void *)(uintptr_t)g_e.tx_buf_phys, 0, txb_pages * 4096);

    g_e.rxd = (struct e1000_rx_desc *)(uintptr_t)g_e.rxd_phys;
    g_e.txd = (struct e1000_tx_desc *)(uintptr_t)g_e.txd_phys;
    g_e.rx_buf = (u8 *)(uintptr_t)g_e.rx_buf_phys;
    g_e.tx_buf = (u8 *)(uintptr_t)g_e.tx_buf_phys;

    for (int i = 0; i < RX_DESC_N; i++) {
        g_e.rxd[i].addr = g_e.rx_buf_phys + (u64)i * RX_BUF_SZ;
        g_e.rxd[i].status = 0;
    }
    for (int i = 0; i < TX_DESC_N; i++) {
        g_e.txd[i].addr = g_e.tx_buf_phys + (u64)i * TX_BUF_SZ;
        g_e.txd[i].status = TDESC_DD; /* free */
    }

    /* RX ring regs */
    ew(REG_RDBAL, (u32)g_e.rxd_phys);
    ew(REG_RDBAH, (u32)(g_e.rxd_phys >> 32));
    ew(REG_RDLEN, RX_DESC_N * (u32)sizeof(struct e1000_rx_desc));
    ew(REG_RDH, 0);
    g_e.rx_tail = RX_DESC_N - 1;
    ew(REG_RDT, g_e.rx_tail);

    /* TX ring regs */
    ew(REG_TDBAL, (u32)g_e.txd_phys);
    ew(REG_TDBAH, (u32)(g_e.txd_phys >> 32));
    ew(REG_TDLEN, TX_DESC_N * (u32)sizeof(struct e1000_tx_desc));
    ew(REG_TDH, 0);
    ew(REG_TDT, 0);
    g_e.tx_tail = 0;

    /* TIPG: QEMU-friendly defaults */
    ew(REG_TIPG, 0x0060200A);

    /* Enable TX: CT=15, COLD=63 (full duplex) */
    ew(REG_TCTL, TCTL_EN | TCTL_PSP | (15u << TCTL_CT_SHIFT) | (63u << TCTL_COLD_SHIFT));

    /* Enable RX: broadcast + strip CRC + 2048 buf */
    ew(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048 | RCTL_UPE | RCTL_MPE);

    g_e.ready = 1;
    kprintf("[net] e1000 ready\n");
    return 0;
}

int e1000_ready(void)
{
    return g_e.ready;
}

void e1000_get_mac(u8 mac[6])
{
    for (int i = 0; i < 6; i++)
        mac[i] = g_e.mac[i];
}

int e1000_send(const void *frame, u32 len)
{
    if (!g_e.ready || !frame || len == 0 || len > TX_BUF_SZ)
        return -1;

    u16 tail = g_e.tx_tail;
    struct e1000_tx_desc *d = &g_e.txd[tail];
    if (!(d->status & TDESC_DD)) {
        /* ring full — try reclaim by reading TDH */
        return -1;
    }

    memcpy(g_e.tx_buf + (u32)tail * TX_BUF_SZ, frame, len);
    d->addr = g_e.tx_buf_phys + (u64)tail * TX_BUF_SZ;
    d->length = (u16)len;
    d->cso = 0;
    d->cmd = TDESC_EOP | TDESC_IFCS | TDESC_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;

    g_e.tx_tail = (u16)((tail + 1) % TX_DESC_N);
    __asm__ volatile("mfence" ::: "memory");
    ew(REG_TDT, g_e.tx_tail);
    return (int)len;
}

int e1000_recv(void *out, u32 buflen)
{
    if (!g_e.ready || !out || !buflen)
        return 0;

    /* Next descriptor to check = (rx_tail + 1) % N */
    u16 next = (u16)((g_e.rx_tail + 1) % RX_DESC_N);
    struct e1000_rx_desc *d = &g_e.rxd[next];
    if (!(d->status & RDESC_DD))
        return 0;

    u32 len = d->length;
    if (len > buflen)
        len = buflen;
    if (len > 0)
        memcpy(out, g_e.rx_buf + (u32)next * RX_BUF_SZ, len);

    d->status = 0;
    g_e.rx_tail = next;
    __asm__ volatile("mfence" ::: "memory");
    ew(REG_RDT, g_e.rx_tail);
    return (int)len;
}
