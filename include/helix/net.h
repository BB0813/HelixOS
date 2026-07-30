#pragma once

#include "helix/types.h"

/* M7 minimal network stack: NIC + eth/ARP/IPv4/ICMP.
 * Static IP 10.0.2.15/24 (QEMU user net). No TCP/DHCP/IPv6.
 *
 * NIC backends (tried in order): e1000, virtio-net-pci.
 */

/* Generic NIC ops used by protocol stack. */
int  nic_init(void);
int  nic_send(const void *frame, u32 len);
int  nic_recv(void *buf, u32 buflen);
int  nic_ready(void);
void nic_get_mac(u8 mac[6]);

/* Concrete drivers (also usable directly). */
int  e1000_init(void);
int  e1000_send(const void *frame, u32 len);
int  e1000_recv(void *buf, u32 buflen);
int  e1000_ready(void);
void e1000_get_mac(u8 mac[6]);

int  virtio_net_init(void);
int  virtio_net_send(const void *frame, u32 len);
int  virtio_net_recv(void *buf, u32 buflen);
int  virtio_net_ready(void);
void virtio_net_get_mac(u8 mac[6]);

/* Protocol stack. */
int  net_init(void);
void net_poll(void);
int  net_ready(void);
u32  net_icmp_echo_replies(void);

static inline u32 net_ipv4(u8 a, u8 b, u8 c, u8 d)
{
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d;
}
