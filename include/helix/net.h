#pragma once

#include "helix/types.h"

/* M7/M8 network: NIC + eth/ARP/IPv4/ICMP + minimal UDP sockets.
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

/* Concrete drivers. */
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
int  net_selftest_ok(void);

/* UDP sockets (Linux AF_INET / SOCK_DGRAM subset). */
#define HELIX_AF_INET     2
#define HELIX_SOCK_DGRAM  2
#define HELIX_SOCK_STREAM 1
#define HELIX_IPPROTO_UDP 17
#define HELIX_IPPROTO_TCP 6

struct helix_sockaddr_in {
    u16 family;   /* AF_INET */
    u16 port;     /* network byte order */
    u32 addr;     /* network byte order IPv4 */
    u8  zero[8];
} __attribute__((packed));

/* Kernel socket object; installed as vfs_file->fs_priv with is_socket. */
struct helix_sock;

struct helix_sock *net_sock_alloc_udp(void);
void net_sock_free(struct helix_sock *s);
int  net_sock_bind(struct helix_sock *s, u32 addr_be, u16 port_be);
int  net_sock_sendto(struct helix_sock *s, const void *data, u32 len,
                     u32 dst_be, u16 port_be);
/* Non-blocking: returns bytes, or negative errno (-EAGAIN if empty). */
int  net_sock_recvfrom(struct helix_sock *s, void *buf, u32 buflen,
                       u32 *src_be, u16 *port_be);
/* Called from IPv4 path. */
void net_udp_input(u32 src_be, u32 dst_be, u16 sport_be, u16 dport_be,
                   const u8 *payload, u32 plen);
int  net_udp_output(u32 dst_be, u16 sport_be, u16 dport_be,
                    const void *data, u32 len);
u32  net_local_ip_be(void);
int  net_is_local_ip_be(u32 addr_be);

static inline u32 net_ipv4(u8 a, u8 b, u8 c, u8 d)
{
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d;
}
