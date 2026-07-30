/*
 * Minimal modern virtio-net-pci driver for QEMU.
 * Single RX + TX queue, polling only (no MSI-X).
 * Spec: Virtual I/O Device (VIRTIO) Version 1.1 — PCI transport.
 */
#include "helix/net.h"
#include "helix/pci.h"
#include "helix/paging.h"
#include "helix/pmm.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/cpuio.h"

#define VIRTIO_VENDOR          0x1AF4u
#define VIRTIO_DEV_NET_LEGACY  0x1000u
#define VIRTIO_DEV_NET_MODERN  0x1041u

/* Vendor-specific PCI capability (ID 0x09) structure types */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* device_status bits */
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED    128

/* feature bits */
#define VIRTIO_F_VERSION_1      32
#define VIRTIO_NET_F_MAC        5
#define VIRTIO_NET_F_STATUS     16
#define VIRTIO_NET_F_MRG_RXBUF  15

/* virtq descriptor flags */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define QSZ 64
#define RX_BUF_SIZE 2048
#define TX_BUF_SIZE 2048

/* virtio_net_hdr (no MRG_RXBUF): 10 bytes */
struct virtio_net_hdr {
    u8  flags;
    u8  gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
} __attribute__((packed));

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[QSZ];
} __attribute__((packed));

struct virtq_used_elem {
    u32 id;
    u32 len;
} __attribute__((packed));

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[QSZ];
} __attribute__((packed));

/* Common config (offsets) — little-endian MMIO */
struct virtio_pci_common_cfg {
    u32 device_feature_select;
    u32 device_feature;
    u32 driver_feature_select;
    u32 driver_feature;
    u16 msix_config;
    u16 num_queues;
    u8  device_status;
    u8  config_generation;
    u16 queue_select;
    u16 queue_size;
    u16 queue_msix_vector;
    u16 queue_enable;
    u16 queue_notify_off;
    u64 queue_desc;
    u64 queue_driver;
    u64 queue_device;
} __attribute__((packed));

struct vq {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    u64 desc_phys, avail_phys, used_phys;
    u16 size;
    u16 last_used;
    u16 free_head;
    u16 num_free;
    u16 notify_off;
    u8 *buf_base;     /* QSZ * BUF_SIZE identity-mapped */
    u64 buf_phys;
    u32 buf_stride;
};

struct virtio_net {
    int ready;
    u8  mac[6];
    volatile struct virtio_pci_common_cfg *common;
    volatile u8  *notify_base;
    u32 notify_multiplier;
    volatile u8  *isr;
    volatile u8  *devcfg;
    struct vq rx, tx;
    u8 bus, dev, func;
};

static struct virtio_net g_vn;

static inline void mmio_w8(volatile void *p, u8 v)  { *(volatile u8 *)p = v; }
static inline void mmio_w16(volatile void *p, u16 v){ *(volatile u16 *)p = v; }
static inline void mmio_w32(volatile void *p, u32 v){ *(volatile u32 *)p = v; }
static inline u8  mmio_r8(volatile void *p)  { return *(volatile u8 *)p; }
static inline u16 mmio_r16(volatile void *p) { return *(volatile u16 *)p; }
static inline u32 mmio_r32(volatile void *p) { return *(volatile u32 *)p; }

static void *map_bar_region(u64 phys, u64 len)
{
    if (!phys || !len)
        return 0;
    /* Round up to page; identity if low, else map MMIO. */
    u64 aligned = phys & ~0xFFFull;
    u64 end = (phys + len + 0xFFFull) & ~0xFFFull;
    if (paging_map_mmio(aligned, end - aligned) != 0)
        return 0;
    return (void *)(uintptr_t)phys;
}

/* Locate modern virtio-net: vendor 1AF4, device 1000 or 1041, with cap common. */
static int find_virtio_net(u8 *bus, u8 *dev, u8 *func)
{
    for (u8 b = 0; b < 32; b++) {
        for (u8 d = 0; d < 32; d++) {
            for (u8 f = 0; f < 8; f++) {
                u16 ven = pci_read16(b, d, f, 0x00);
                if (ven != VIRTIO_VENDOR)
                    continue;
                u16 did = pci_read16(b, d, f, 0x02);
                if (did != VIRTIO_DEV_NET_LEGACY && did != VIRTIO_DEV_NET_MODERN)
                    continue;
                *bus = b; *dev = d; *func = f;
                return 0;
            }
        }
    }
    return -1;
}

struct cap_info {
    u8  cfg_type;
    u8  bar;
    u32 offset;
    u32 length;
};

static int read_vndr_cap(u8 bus, u8 dev, u8 func, u8 cap_off, struct cap_info *out)
{
    /* virtio_pci_cap: cap_vndr, next, len, cfg_type, bar, padding[3], offset, length */
    out->cfg_type = pci_read8(bus, dev, func, (u8)(cap_off + 3));
    out->bar      = pci_read8(bus, dev, func, (u8)(cap_off + 4));
    out->offset   = pci_read32(bus, dev, func, (u8)(cap_off + 8));
    out->length   = pci_read32(bus, dev, func, (u8)(cap_off + 12));
    return 0;
}

static void *cap_map(u8 bus, u8 dev, u8 func, const struct cap_info *c)
{
    if (c->bar > 5)
        return 0;
    u8 bar_off = (u8)(0x10 + c->bar * 4);
    /* 64-bit BARs: if previous is 64-bit high half, skip — QEMU uses BAR0/1 typically */
    u32 lo = pci_read32(bus, dev, func, bar_off);
    if (lo & 1u)
        return 0; /* I/O */
    u64 base = lo & ~0xFull;
    if ((lo & 0x6) == 0x4) {
        u32 hi = pci_read32(bus, dev, func, (u8)(bar_off + 4));
        base |= (u64)hi << 32;
    }
    if (!base)
        return 0;
    u64 map_len = c->offset + c->length;
    if (map_len < 0x1000)
        map_len = 0x1000;
    return map_bar_region(base + c->offset, c->length ? c->length : 0x1000);
}

static int vq_alloc(struct vq *q, u32 buf_stride)
{
    memset(q, 0, sizeof(*q));
    q->size = QSZ;
    q->buf_stride = buf_stride;
    q->num_free = QSZ;
    q->free_head = 0;
    q->last_used = 0;

    /* One page each for desc / avail / used is enough for QSZ=64. */
    u64 d = pmm_alloc_page();
    u64 a = pmm_alloc_page();
    u64 u = pmm_alloc_page();
    if (!d || !a || !u)
        return -1;
    memset((void *)(uintptr_t)d, 0, 4096);
    memset((void *)(uintptr_t)a, 0, 4096);
    memset((void *)(uintptr_t)u, 0, 4096);
    q->desc_phys = d;
    q->avail_phys = a;
    q->used_phys = u;
    q->desc  = (struct virtq_desc *)(uintptr_t)d;
    q->avail = (struct virtq_avail *)(uintptr_t)a;
    q->used  = (struct virtq_used *)(uintptr_t)u;

    /* Free list chain */
    for (u16 i = 0; i < QSZ - 1; i++)
        q->desc[i].next = (u16)(i + 1);
    q->desc[QSZ - 1].next = 0xFFFF;

    /* Packet buffers: QSZ * stride, page-aligned contiguous. */
    u64 nbytes = (u64)QSZ * buf_stride;
    u64 npages = (nbytes + 4095) / 4096;
    u64 bp = pmm_alloc_pages(npages);
    if (!bp)
        return -1;
    memset((void *)(uintptr_t)bp, 0, npages * 4096);
    q->buf_phys = bp;
    q->buf_base = (u8 *)(uintptr_t)bp;
    return 0;
}

static u16 vq_alloc_desc(struct vq *q)
{
    if (!q->num_free)
        return 0xFFFF;
    u16 i = q->free_head;
    q->free_head = q->desc[i].next;
    q->num_free--;
    q->desc[i].next = 0;
    q->desc[i].flags = 0;
    return i;
}

static void vq_free_desc(struct vq *q, u16 i)
{
    q->desc[i].next = q->free_head;
    q->free_head = i;
    q->num_free++;
}

static void mmio_w64_split(volatile void *p, u64 v)
{
    /* Two 32-bit LE stores — safer on some MMIO windows than one u64. */
    mmio_w32(p, (u32)v);
    mmio_w32((volatile u8 *)p + 4, (u32)(v >> 32));
}

static void vq_notify(struct virtio_net *vn, struct vq *q, u16 queue_idx)
{
    if (!vn->notify_base)
        return;
    u32 off = (u32)q->notify_off * vn->notify_multiplier;
    __asm__ volatile("mfence" ::: "memory");
    /* Modern transport: write queue index to notify address. */
    mmio_w16(vn->notify_base + off, queue_idx);
}

static void setup_queue(struct virtio_net *vn, struct vq *q, u16 index)
{
    volatile struct virtio_pci_common_cfg *c = vn->common;
    mmio_w16(&c->queue_select, index);
    /* Read device-offered size; clamp to our QSZ. */
    u16 dev_sz = mmio_r16(&c->queue_size);
    if (dev_sz == 0)
        dev_sz = QSZ;
    if (dev_sz > QSZ)
        dev_sz = QSZ;
    /* force power-of-two we allocated for */
    q->size = QSZ;
    mmio_w16(&c->queue_size, q->size);
    mmio_w16(&c->queue_msix_vector, 0xFFFF); /* no MSI-X */
    mmio_w64_split(&c->queue_desc, q->desc_phys);
    mmio_w64_split(&c->queue_driver, q->avail_phys);
    mmio_w64_split(&c->queue_device, q->used_phys);
    q->notify_off = mmio_r16(&c->queue_notify_off);
    __asm__ volatile("mfence" ::: "memory");
    mmio_w16(&c->queue_enable, 1);
    kprintf("[net] q%u size=%u notify_off=%u desc=0x%llx\n",
            (unsigned)index, (unsigned)q->size, (unsigned)q->notify_off,
            (unsigned long long)q->desc_phys);
    (void)dev_sz;
}

static void rx_refill_all(struct virtio_net *vn)
{
    struct vq *q = &vn->rx;
    while (q->num_free) {
        u16 d = vq_alloc_desc(q);
        if (d == 0xFFFF)
            break;
        q->desc[d].addr  = q->buf_phys + (u64)d * q->buf_stride;
        q->desc[d].len   = q->buf_stride;
        q->desc[d].flags = VIRTQ_DESC_F_WRITE;
        q->desc[d].next  = 0;

        u16 slot = (u16)(q->avail->idx % QSZ);
        q->avail->ring[slot] = d;
        __asm__ volatile("mfence" ::: "memory");
        q->avail->idx++;
    }
    vq_notify(vn, q, 0); /* RX = queue 0 */
}

int virtio_net_init(void)
{
    memset(&g_vn, 0, sizeof(g_vn));

    u8 bus, dev, func;
    if (find_virtio_net(&bus, &dev, &func) != 0) {
        kprintf("[net] virtio-net-pci not found\n");
        return -1;
    }
    g_vn.bus = bus; g_vn.dev = dev; g_vn.func = func;
    kprintf("[net] virtio-net at %x:%x.%u\n",
            (unsigned)bus, (unsigned)dev, (unsigned)func);

    pci_enable_bus_master(bus, dev, func);

    /* Walk PCI capabilities for virtio vendor caps. */
    u16 status = pci_read16(bus, dev, func, 0x06);
    if (!(status & (1u << 4))) {
        kprintf("[net] no PCI caps\n");
        return -1;
    }
    u8 cap = pci_read8(bus, dev, func, 0x34);
    struct cap_info common_c = {0}, notify_c = {0}, isr_c = {0}, dev_c = {0};
    int got_common = 0, got_notify = 0, got_isr = 0, got_dev = 0;
    u32 notify_mult = 0;

    for (int guard = 0; cap && cap != 0xFF && guard < 64; guard++) {
        u8 id = pci_read8(bus, dev, func, cap);
        u8 next = pci_read8(bus, dev, func, (u8)(cap + 1));
        if (id == 0x09) {
            struct cap_info ci;
            read_vndr_cap(bus, dev, func, cap, &ci);
            switch (ci.cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                common_c = ci; got_common = 1; break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                notify_c = ci; got_notify = 1;
                /* notify_off_multiplier at cap+16 */
                notify_mult = pci_read32(bus, dev, func, (u8)(cap + 16));
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                isr_c = ci; got_isr = 1; break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                dev_c = ci; got_dev = 1; break;
            default: break;
            }
        }
        cap = next;
    }

    if (!got_common || !got_notify) {
        kprintf("[net] modern virtio caps missing (common=%d notify=%d)\n",
                got_common, got_notify);
        return -1;
    }

    g_vn.common = (volatile struct virtio_pci_common_cfg *)cap_map(bus, dev, func, &common_c);
    g_vn.notify_base = (volatile u8 *)cap_map(bus, dev, func, &notify_c);
    g_vn.notify_multiplier = notify_mult ? notify_mult : 1;
    if (got_isr)
        g_vn.isr = (volatile u8 *)cap_map(bus, dev, func, &isr_c);
    if (got_dev)
        g_vn.devcfg = (volatile u8 *)cap_map(bus, dev, func, &dev_c);

    if (!g_vn.common || !g_vn.notify_base) {
        kprintf("[net] map common/notify failed\n");
        return -1;
    }
    kprintf("[net] common=%p notify=%p mult=%u\n",
            (void *)g_vn.common, (void *)g_vn.notify_base,
            (unsigned)g_vn.notify_multiplier);

    volatile struct virtio_pci_common_cfg *cfg = g_vn.common;

    /* Reset */
    mmio_w8(&cfg->device_status, 0);
    for (volatile int i = 0; i < 10000; i++)
        ;
    mmio_w8(&cfg->device_status, VIRTIO_STATUS_ACK);
    mmio_w8(&cfg->device_status,
            (u8)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    /* Negotiate features: read device features page 0 + 1 */
    mmio_w32(&cfg->device_feature_select, 0);
    u32 f0 = mmio_r32(&cfg->device_feature);
    mmio_w32(&cfg->device_feature_select, 1);
    u32 f1 = mmio_r32(&cfg->device_feature);

    u32 want0 = 0;
    if (f0 & (1u << VIRTIO_NET_F_MAC))
        want0 |= (1u << VIRTIO_NET_F_MAC);
    if (f0 & (1u << VIRTIO_NET_F_STATUS))
        want0 |= (1u << VIRTIO_NET_F_STATUS);
    /* Do NOT enable MRG_RXBUF — keeps hdr at 10 bytes. */
    u32 want1 = 0;
    if (f1 & (1u << (VIRTIO_F_VERSION_1 - 32)))
        want1 |= (1u << (VIRTIO_F_VERSION_1 - 32));

    mmio_w32(&cfg->driver_feature_select, 0);
    mmio_w32(&cfg->driver_feature, want0);
    mmio_w32(&cfg->driver_feature_select, 1);
    mmio_w32(&cfg->driver_feature, want1);

    mmio_w8(&cfg->device_status,
            (u8)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK));
    u8 st = mmio_r8(&cfg->device_status);
    if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[net] FEATURES_OK rejected (status=0x%x)\n", st);
        mmio_w8(&cfg->device_status, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* MAC from device config */
    if (g_vn.devcfg && (want0 & (1u << VIRTIO_NET_F_MAC))) {
        for (int i = 0; i < 6; i++)
            g_vn.mac[i] = g_vn.devcfg[i];
    } else {
        /* Fallback QEMU-ish */
        g_vn.mac[0] = 0x52; g_vn.mac[1] = 0x54; g_vn.mac[2] = 0x00;
        g_vn.mac[3] = 0x12; g_vn.mac[4] = 0x34; g_vn.mac[5] = 0x56;
    }
    kprintf("[net] MAC %x:%x:%x:%x:%x:%x\n",
            g_vn.mac[0], g_vn.mac[1], g_vn.mac[2],
            g_vn.mac[3], g_vn.mac[4], g_vn.mac[5]);

    if (vq_alloc(&g_vn.rx, RX_BUF_SIZE) != 0 ||
        vq_alloc(&g_vn.tx, TX_BUF_SIZE) != 0) {
        kprintf("[net] vq alloc failed\n");
        return -1;
    }

    setup_queue(&g_vn, &g_vn.rx, 0);
    setup_queue(&g_vn, &g_vn.tx, 1);
    rx_refill_all(&g_vn);

    mmio_w8(&cfg->device_status,
            (u8)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK));

    g_vn.ready = 1;
    kprintf("[net] virtio-net ready queues=%u\n",
            (unsigned)mmio_r16(&cfg->num_queues));
    return 0;
}

int virtio_net_ready(void)
{
    return g_vn.ready;
}

void virtio_net_get_mac(u8 mac[6])
{
    for (int i = 0; i < 6; i++)
        mac[i] = g_vn.mac[i];
}

int virtio_net_send(const void *frame, u32 len)
{
    if (!g_vn.ready || !frame || len == 0 || len > (TX_BUF_SIZE - sizeof(struct virtio_net_hdr)))
        return -1;

    struct vq *q = &g_vn.tx;

    /* Reclaim completed TX — used->idx written by device */
    u16 used_idx = *(volatile u16 *)&q->used->idx;
    while (q->last_used != used_idx) {
        u16 slot = (u16)(q->last_used % QSZ);
        u16 id = (u16)q->used->ring[slot].id;
        vq_free_desc(q, id);
        q->last_used++;
        used_idx = *(volatile u16 *)&q->used->idx;
    }

    u16 d = vq_alloc_desc(q);
    if (d == 0xFFFF) {
        kprintf("[net] tx full\n");
        return -1;
    }

    u8 *buf = q->buf_base + (u32)d * q->buf_stride;
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(buf + sizeof(*hdr), frame, len);

    q->desc[d].addr  = q->buf_phys + (u64)d * q->buf_stride;
    q->desc[d].len   = (u32)(sizeof(*hdr) + len);
    q->desc[d].flags = 0;
    q->desc[d].next  = 0;

    u16 slot = (u16)(*(volatile u16 *)&q->avail->idx % QSZ);
    q->avail->ring[slot] = d;
    __asm__ volatile("mfence" ::: "memory");
    *(volatile u16 *)&q->avail->idx = (u16)(*(volatile u16 *)&q->avail->idx + 1);
    vq_notify(&g_vn, q, 1);
    return (int)len;
}

int virtio_net_recv(void *out, u32 buflen)
{
    if (!g_vn.ready || !out || !buflen)
        return 0;

    struct vq *q = &g_vn.rx;
    u16 used_idx = *(volatile u16 *)&q->used->idx;
    if (q->last_used == used_idx)
        return 0;

    u16 slot = (u16)(q->last_used % QSZ);
    u16 id  = (u16)q->used->ring[slot].id;
    u32 plen = q->used->ring[slot].len;
    q->last_used++;

    u8 *buf = q->buf_base + (u32)id * q->buf_stride;
    u32 hdrsz = (u32)sizeof(struct virtio_net_hdr);
    int ret = 0;
    if (plen > hdrsz) {
        u32 flen = plen - hdrsz;
        if (flen > buflen)
            flen = buflen;
        memcpy(out, buf + hdrsz, flen);
        ret = (int)flen;
    }

    q->desc[id].addr  = q->buf_phys + (u64)id * q->buf_stride;
    q->desc[id].len   = q->buf_stride;
    q->desc[id].flags = VIRTQ_DESC_F_WRITE;
    q->desc[id].next  = 0;
    {
        u16 as = (u16)(*(volatile u16 *)&q->avail->idx % QSZ);
        q->avail->ring[as] = id;
        __asm__ volatile("mfence" ::: "memory");
        *(volatile u16 *)&q->avail->idx = (u16)(*(volatile u16 *)&q->avail->idx + 1);
    }
    vq_notify(&g_vn, q, 0);

    if (g_vn.isr)
        (void)mmio_r8(g_vn.isr);

    return ret;
}
