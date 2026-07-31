/*
 * Minimal minimal network stack for M7: Ethernet + ARP + IPv4 + ICMP.
 * Self-test: ARP for gateway + ICMP echo to gateway. On reply print HelixNetOK.
 */
#include "helix/net.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/timer.h"
#include "helix/types.h"

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP   0x0800
#define ARP_OP_REQ    1
#define ARP_OP_REP    2
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define ICMP_ECHO     8
#define ICMP_ECHOREPLY 0

/* Host-order constants (compile-time). */
#define NET_IP_SELF    0x0A00020Fu  /* 10.0.2.15 */
#define NET_IP_GW      0x0A000202u  /* 10.0.2.2 */
#define NET_IP_MASK    0xFFFFFF00u  /* 255.255.255.0 */

struct eth_hdr {
    u8  dst[6];
    u8  src[6];
    u16 type; /* big-endian */
} __attribute__((packed));

struct arp_pkt {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 oper;
    u8  sha[6];
    u8  spa[4];
    u8  tha[6];
    u8  tpa[4];
} __attribute__((packed));

struct ip_hdr {
    u8  ver_ihl;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 frag_off;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u32 src;
    u32 dst;
} __attribute__((packed));

struct icmp_hdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed));

static int g_ready;
static u8  g_mac[6];
static u32 g_ip = NET_IP_SELF;
static u32 g_gw = NET_IP_GW;
static u8  g_gw_mac[6];
static int g_gw_mac_valid;
static u32 g_icmp_replies;
static u32 g_icmp_echo_rx;
static u32 g_self_echo_ok;
static u16 g_ip_id;
static u16 g_icmp_seq;
static u64 g_last_arp_tick;
static u64 g_last_ping_tick;
static int g_selftest_done;
static int g_announced;

static u16 htons_u16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u32 htons_u32(u32 x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8)  |
           ((x & 0x00FF0000u) >> 8)  |
           ((x & 0xFF000000u) >> 24);
}
#define ntohs_u16 htons_u16
#define ntohs_u32 htons_u32

static u16 checksum16(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void ip_to_bytes(u32 ip, u8 out[4])
{
    out[0] = (u8)(ip >> 24);
    out[1] = (u8)(ip >> 16);
    out[2] = (u8)(ip >> 8);
    out[3] = (u8)ip;
}

static u32 bytes_to_ip(const u8 b[4])
{
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static int eth_send(const u8 dst[6], u16 ethertype, const void *payload, u32 plen)
{
    u8 frame[1518];
    if (plen + sizeof(struct eth_hdr) > sizeof(frame))
        return -1;
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    for (int i = 0; i < 6; i++) {
        eh->dst[i] = dst[i];
        eh->src[i] = g_mac[i];
    }
    eh->type = htons_u16(ethertype);
    memcpy(frame + sizeof(*eh), payload, plen);
    return nic_send(frame, (u32)(sizeof(*eh) + plen));
}

static void arp_send(u16 oper, const u8 tha[6], u32 tpa)
{
    struct arp_pkt ap;
    memset(&ap, 0, sizeof(ap));
    ap.htype = htons_u16(1);
    ap.ptype = htons_u16(ETH_TYPE_IP);
    ap.hlen = 6;
    ap.plen = 4;
    ap.oper = htons_u16(oper);
    for (int i = 0; i < 6; i++)
        ap.sha[i] = g_mac[i];
    ip_to_bytes(g_ip, ap.spa);
    if (tha) {
        for (int i = 0; i < 6; i++)
            ap.tha[i] = tha[i];
    }
    ip_to_bytes(tpa, ap.tpa);

    u8 dst[6];
    if (oper == ARP_OP_REQ) {
        for (int i = 0; i < 6; i++)
            dst[i] = 0xFF;
    } else {
        for (int i = 0; i < 6; i++)
            dst[i] = tha[i];
    }
    eth_send(dst, ETH_TYPE_ARP, &ap, (u32)sizeof(ap));
}

static void handle_arp(const u8 *pkt, u32 len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *ap = (const struct arp_pkt *)pkt;
    if (ntohs_u16(ap->htype) != 1 || ntohs_u16(ap->ptype) != ETH_TYPE_IP)
        return;
    if (ap->hlen != 6 || ap->plen != 4)
        return;
    u16 op = ntohs_u16(ap->oper);
    u32 spa = bytes_to_ip(ap->spa);
    u32 tpa = bytes_to_ip(ap->tpa);

    if (spa == g_gw) {
        for (int i = 0; i < 6; i++)
            g_gw_mac[i] = ap->sha[i];
        g_gw_mac_valid = 1;
    }

    if (op == ARP_OP_REQ && tpa == g_ip) {
        kprintf("[arp] who-has %u.%u.%u.%u tell %u.%u.%u.%u -> reply\n",
                (unsigned)(tpa >> 24), (unsigned)((tpa >> 16) & 255),
                (unsigned)((tpa >> 8) & 255), (unsigned)(tpa & 255),
                (unsigned)(spa >> 24), (unsigned)((spa >> 16) & 255),
                (unsigned)((spa >> 8) & 255), (unsigned)(spa & 255));
        arp_send(ARP_OP_REP, ap->sha, spa);
    } else if (op == ARP_OP_REP && tpa == g_ip) {
        kprintf("[arp] reply %u.%u.%u.%u is-at %x:%x:%x:%x:%x:%x\n",
                (unsigned)(spa >> 24), (unsigned)((spa >> 16) & 255),
                (unsigned)((spa >> 8) & 255), (unsigned)(spa & 255),
                ap->sha[0], ap->sha[1], ap->sha[2],
                ap->sha[3], ap->sha[4], ap->sha[5]);
        if (spa == g_gw) {
            for (int i = 0; i < 6; i++)
                g_gw_mac[i] = ap->sha[i];
            g_gw_mac_valid = 1;
        }
    }
}

static int ip_send(u32 dst_ip, u8 proto, const void *payload, u32 plen)
{
    u8 buf[1500];
    if (sizeof(struct ip_hdr) + plen > sizeof(buf))
        return -1;

    u32 l2_ip = ((dst_ip & NET_IP_MASK) == (g_ip & NET_IP_MASK)) ? dst_ip : g_gw;
    u8 dst_mac[6];
    if (l2_ip == g_gw && g_gw_mac_valid) {
        for (int i = 0; i < 6; i++)
            dst_mac[i] = g_gw_mac[i];
    } else if (l2_ip == g_gw) {
        arp_send(ARP_OP_REQ, 0, g_gw);
        return -1;
    } else {
        arp_send(ARP_OP_REQ, 0, l2_ip);
        return -1;
    }

    struct ip_hdr *ip = (struct ip_hdr *)buf;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons_u16((u16)(sizeof(*ip) + plen));
    ip->id = htons_u16(++g_ip_id);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = htons_u32(g_ip);
    ip->dst = htons_u32(dst_ip);
    ip->checksum = 0;
    ip->checksum = htons_u16(checksum16(ip, sizeof(*ip)));
    memcpy(buf + sizeof(*ip), payload, plen);
    return eth_send(dst_mac, ETH_TYPE_IP, buf, (u32)(sizeof(*ip) + plen));
}

static void icmp_send_echo(u32 dst, u16 id, u16 seq, const void *data, u32 dlen)
{
    u8 buf[64];
    if (sizeof(struct icmp_hdr) + dlen > sizeof(buf))
        dlen = (u32)(sizeof(buf) - sizeof(struct icmp_hdr));
    struct icmp_hdr *ic = (struct icmp_hdr *)buf;
    ic->type = ICMP_ECHO;
    ic->code = 0;
    ic->checksum = 0;
    ic->id = htons_u16(id);
    ic->seq = htons_u16(seq);
    if (dlen)
        memcpy(buf + sizeof(*ic), data, dlen);
    else {
        const char *msg = "HelixPing";
        dlen = 9;
        memcpy(buf + sizeof(*ic), msg, dlen);
    }
    u32 total = (u32)sizeof(*ic) + dlen;
    ic->checksum = htons_u16(checksum16(buf, total));
    ip_send(dst, IP_PROTO_ICMP, buf, total);
}

static void icmp_send_reply(u32 dst, u16 id, u16 seq, const void *data, u32 dlen)
{
    u8 buf[576];
    if (sizeof(struct icmp_hdr) + dlen > sizeof(buf))
        dlen = (u32)(sizeof(buf) - sizeof(struct icmp_hdr));
    struct icmp_hdr *ic = (struct icmp_hdr *)buf;
    ic->type = ICMP_ECHOREPLY;
    ic->code = 0;
    ic->checksum = 0;
    ic->id = id;
    ic->seq = seq;
    memcpy(buf + sizeof(*ic), data, dlen);
    u32 total = (u32)sizeof(*ic) + dlen;
    ic->checksum = htons_u16(checksum16(buf, total));
    if (ip_send(dst, IP_PROTO_ICMP, buf, total) >= 0) {
        g_icmp_echo_rx++;
        g_icmp_replies++;
        kprintf("[icmp] ICMP echo reply -> %u.%u.%u.%u id=%u seq=%u\n",
                (unsigned)(dst >> 24), (unsigned)((dst >> 16) & 255),
                (unsigned)((dst >> 8) & 255), (unsigned)(dst & 255),
                (unsigned)ntohs_u16(id), (unsigned)ntohs_u16(seq));
    }
}

static void handle_icmp(u32 src_ip, const u8 *pkt, u32 len)
{
    if (len < sizeof(struct icmp_hdr))
        return;
    const struct icmp_hdr *ic = (const struct icmp_hdr *)pkt;
    if (ic->type == ICMP_ECHO) {
        icmp_send_reply(src_ip, ic->id, ic->seq,
                        pkt + sizeof(*ic), len - (u32)sizeof(*ic));
    } else if (ic->type == ICMP_ECHOREPLY) {
        kprintf("[icmp] echo reply from %u.%u.%u.%u seq=%u\n",
                (unsigned)(src_ip >> 24), (unsigned)((src_ip >> 16) & 255),
                (unsigned)((src_ip >> 8) & 255), (unsigned)(src_ip & 255),
                (unsigned)ntohs_u16(ic->seq));
        g_icmp_replies++;
        if (src_ip == g_gw)
            g_self_echo_ok = 1;
    }
}

static void handle_ip(const u8 *pkt, u32 len)
{
    if (len < sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    if ((ip->ver_ihl >> 4) != 4)
        return;
    u32 ihl = (ip->ver_ihl & 0xF) * 4u;
    if (ihl < 20 || len < ihl)
        return;
    u32 total = ntohs_u16(ip->total_len);
    if (total < ihl || total > len)
        total = len;
    u32 src = ntohs_u32(ip->src);
    u32 dst = ntohs_u32(ip->dst);
    if (dst != g_ip && dst != 0xFFFFFFFFu)
        return;

    if (ip->proto == IP_PROTO_ICMP)
        handle_icmp(src, pkt + ihl, total - ihl);
    else if (ip->proto == IP_PROTO_UDP && total > ihl + 8) {
        /* Minimal UDP demux: [src_port_be(2), dst_port_be(2), len(2), csum(2)] */
        const u8 *udp = pkt + ihl;
        u16 sport_be = (u16)udp[0] | ((u16)udp[1] << 8);
        u16 dport_be = (u16)udp[2] | ((u16)udp[3] << 8);
        u16 udp_len  = (u16)udp[4] | ((u16)udp[5] << 8);
        u32 plen = (udp_len > 8) ? (u32)(udp_len - 8) : 0;
        const u8 *payload = udp + 8;
        if (plen > (total - ihl - 8))
            plen = total - ihl - 8;
        net_udp_input(src, dst, sport_be, dport_be, payload, plen);
    }
}

static void handle_frame(const u8 *frame, u32 len)
{
    if (len < sizeof(struct eth_hdr))
        return;
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;
    u16 type = ntohs_u16(eh->type);
    const u8 *payload = frame + sizeof(*eh);
    u32 plen = len - (u32)sizeof(*eh);
    if (type == ETH_TYPE_ARP)
        handle_arp(payload, plen);
    else if (type == ETH_TYPE_IP)
        handle_ip(payload, plen);
}

int net_init(void)
{
    g_ready = 0;
    g_gw_mac_valid = 0;
    g_icmp_replies = 0;
    g_icmp_echo_rx = 0;
    g_self_echo_ok = 0;
    g_selftest_done = 0;
    g_announced = 0;
    g_ip_id = 1;
    g_icmp_seq = 1;
    g_last_arp_tick = 0;
    g_last_ping_tick = 0;

    if (nic_init() != 0)
        return -1;
    nic_get_mac(g_mac);
    g_ready = 1;

    kprintf("[net] M7 net ready ip=%u.%u.%u.%u/24 gw=%u.%u.%u.%u\n",
            (unsigned)(g_ip >> 24), (unsigned)((g_ip >> 16) & 255),
            (unsigned)((g_ip >> 8) & 255), (unsigned)(g_ip & 255),
            (unsigned)(g_gw >> 24), (unsigned)((g_gw >> 16) & 255),
            (unsigned)((g_gw >> 8) & 255), (unsigned)(g_gw & 255));
    kprintf("[net] 10.0.2.15 configured (QEMU user net)\n");

    arp_send(ARP_OP_REQ, 0, g_gw);
    g_last_arp_tick = timer_ticks();
    return 0;
}

int net_ready(void)
{
    return g_ready;
}

u32 net_icmp_echo_replies(void)
{
    return g_icmp_replies;
}

u32 net_local_ip_be(void)
{
    /* g_ip is host order; swap to network order */
    return ((g_ip & 0xFF) << 24) | ((g_ip >> 8 & 0xFF) << 16) |
           ((g_ip >> 16 & 0xFF) << 8) | (g_ip >> 24);
}

int net_is_local_ip_be(u32 addr_be)
{
    return addr_be == net_local_ip_be() || addr_be == 0;
}

/* Build a UDP/IP packet and send it on the wire. */
int net_udp_output(u32 dst_be, u16 sport_be, u16 dport_be,
                   const void *data, u32 len)
{
    if (!g_ready)
        return -1;
    /* UDP header: src_port, dst_port, length, checksum(0) */
    u8 pkt[1500];
    if (8 + len > sizeof(pkt) - sizeof(struct ip_hdr) - sizeof(struct eth_hdr))
        return -1;
    struct {
        u16 sport, dport, udplen, csum;
    } *uh = (void *)pkt;
    uh->sport  = sport_be;
    uh->dport  = dport_be;
    uh->udplen = htons_u16((u16)(8 + len));
    uh->csum   = 0;
    memcpy(pkt + 8, data, len);

    /* Convert dst_be (network order) to host order for ip_send */
    u32 dst_host = ((dst_be & 0xFF) << 24) | ((dst_be >> 8 & 0xFF) << 16) |
                   ((dst_be >> 16 & 0xFF) << 8) | (dst_be >> 24);
    return ip_send(dst_host, IP_PROTO_UDP, pkt, 8 + len);
}

void net_poll(void)
{
    if (!g_ready)
        return;

    u8 frame[2048];
    for (int n = 0; n < 16; n++) {
        int nread = nic_recv(frame, sizeof(frame));
        if (nread <= 0)
            break;
        handle_frame(frame, (u32)nread);
    }

    u64 now = timer_ticks();
    u32 hz = timer_hz() ? timer_hz() : 100;

    if (!g_gw_mac_valid && now - g_last_arp_tick >= hz) {
        arp_send(ARP_OP_REQ, 0, g_gw);
        g_last_arp_tick = now;
    }

    if (g_gw_mac_valid && !g_self_echo_ok) {
        if (g_last_ping_tick == 0 || now - g_last_ping_tick >= hz) {
            kprintf("[icmp] ping %u.%u.%u.%u seq=%u\n",
                    (unsigned)(g_gw >> 24), (unsigned)((g_gw >> 16) & 255),
                    (unsigned)((g_gw >> 8) & 255), (unsigned)(g_gw & 255),
                    (unsigned)g_icmp_seq);
            icmp_send_echo(g_gw, 0x4845 /* HE */, g_icmp_seq, 0, 0);
            g_icmp_seq++;
            g_last_ping_tick = now;
        }
    }

    if (g_self_echo_ok && !g_announced) {
        g_announced = 1;
        g_selftest_done = 1;
        kprintf("[net] ICMP echo reply OK (gateway)\n");
        kprintf("[net] HelixNetOK\n");
    }
}
