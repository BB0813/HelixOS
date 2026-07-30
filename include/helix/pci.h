#pragma once

#include "helix/types.h"
#include "helix/cpuio.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline u32 pci_cfg_addr(u8 bus, u8 dev, u8 func, u8 offset)
{
    return (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)func << 8) | (offset & 0xFC);
}

static inline u32 pci_read32(u8 bus, u8 dev, u8 func, u8 offset)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static inline void pci_write32(u8 bus, u8 dev, u8 func, u8 offset, u32 val)
{
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

static inline u16 pci_read16(u8 bus, u8 dev, u8 func, u8 offset)
{
    u32 v = pci_read32(bus, dev, func, offset & 0xFC);
    return (u16)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

static inline void pci_write16(u8 bus, u8 dev, u8 func, u8 offset, u16 val)
{
    u32 cur = pci_read32(bus, dev, func, offset & 0xFC);
    u32 shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((u32)val << shift);
    pci_write32(bus, dev, func, offset & 0xFC, cur);
}

static inline u8 pci_read8(u8 bus, u8 dev, u8 func, u8 offset)
{
    u32 v = pci_read32(bus, dev, func, offset & 0xFC);
    return (u8)((v >> ((offset & 3) * 8)) & 0xFF);
}

static inline void pci_write8(u8 bus, u8 dev, u8 func, u8 offset, u8 val)
{
    u32 cur = pci_read32(bus, dev, func, offset & 0xFC);
    u32 shift = (offset & 3) * 8;
    cur = (cur & ~(0xFFu << shift)) | ((u32)val << shift);
    pci_write32(bus, dev, func, offset & 0xFC, cur);
}

/* Enable I/O + memory + bus master. */
static inline void pci_enable_bus_master(u8 bus, u8 dev, u8 func)
{
    u16 cmd = pci_read16(bus, dev, func, 0x04);
    cmd |= 0x0007; /* IO | MEM | BusMaster */
    pci_write16(bus, dev, func, 0x04, cmd);
}

/* Read 32/64-bit memory BAR at given config offset (0x10, 0x14, ...). */
static inline u64 pci_read_bar(u8 bus, u8 dev, u8 func, u8 bar_off, u64 *size_hint)
{
    u32 lo = pci_read32(bus, dev, func, bar_off);
    if (lo == 0 || lo == 0xFFFFFFFFu)
        return 0;
    if (lo & 1u) {
        /* I/O BAR — not used for virtio MMIO */
        if (size_hint)
            *size_hint = 0;
        return lo & ~0x3u;
    }
    u64 addr = lo & ~0xFull;
    if ((lo & 0x6) == 0x4) {
        u32 hi = pci_read32(bus, dev, func, (u8)(bar_off + 4));
        addr |= (u64)hi << 32;
    }
    if (size_hint)
        *size_hint = 0; /* size probe optional */
    return addr;
}
