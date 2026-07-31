/*
 * M8 minimal UDP sockets.
 * - Up to 8 UDP sockets (global table; not per-task for simplicity)
 * - RX queue per socket (8 packets × 512 B payload)
 * - Local delivery when dst is self; else L3 send via net_udp_output
 * - Non-blocking recv (returns 0 / -EAGAIN when empty)
 */
#include "helix/net.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/errno.h"
#include "helix/heap.h"

#define UDP_SOCK_MAX   8
#define UDP_RXQ_DEPTH  8
#define UDP_RX_PAY     512

struct udp_rx_pkt {
    u32 src_be;   /* network order */
    u16 sport_be;
    u16 len;
    u8  data[UDP_RX_PAY];
};

struct helix_sock {
    int  used;
    int  bound;
    u32  laddr_be; /* network order; 0 = INADDR_ANY */
    u16  lport_be; /* network order */
    u16  rx_head, rx_tail, rx_count;
    struct udp_rx_pkt rxq[UDP_RXQ_DEPTH];
};

static struct helix_sock g_socks[UDP_SOCK_MAX];
static u16 g_ephemeral = 40000; /* host order counter */

/* Provided by net.c */
extern int  net_udp_output(u32 dst_be, u16 sport_be, u16 dport_be,
                           const void *data, u32 len);
extern u32  net_local_ip_be(void); /* network-order self IP */
extern int  net_is_local_ip_be(u32 addr_be);

struct helix_sock *net_sock_alloc_udp(void)
{
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].used = 1;
            return &g_socks[i];
        }
    }
    return 0;
}

void net_sock_free(struct helix_sock *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
}

int net_sock_bind(struct helix_sock *s, u32 addr_be, u16 port_be)
{
    if (!s || !s->used)
        return -EINVAL;
    /* port 0 → pick ephemeral */
    if (port_be == 0) {
        u16 p = g_ephemeral++;
        if (g_ephemeral < 40000)
            g_ephemeral = 40000;
        port_be = (u16)((p >> 8) | (p << 8)); /* htons */
    }
    /* Reject if another socket already bound to same port (any/local). */
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        struct helix_sock *o = &g_socks[i];
        if (!o->used || !o->bound || o == s)
            continue;
        if (o->lport_be == port_be) {
            /* Allow if both on distinct non-any addresses — skip for M8. */
            return -EADDRINUSE;
        }
    }
    s->laddr_be = addr_be;
    s->lport_be = port_be;
    s->bound = 1;
    return 0;
}

static void ensure_bound(struct helix_sock *s)
{
    if (!s->bound)
        (void)net_sock_bind(s, 0, 0);
}

int net_sock_sendto(struct helix_sock *s, const void *data, u32 len,
                    u32 dst_be, u16 dport_be)
{
    if (!s || !s->used || !data)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (len > 1400)
        len = 1400;
    ensure_bound(s);

    /* Local delivery: dst is self or 127.0.0.1 */
    if (net_is_local_ip_be(dst_be) || dst_be == 0x0100007Fu /* 127.0.0.1 BE */) {
        /* Find socket bound to dport */
        for (int i = 0; i < UDP_SOCK_MAX; i++) {
            struct helix_sock *dst = &g_socks[i];
            if (!dst->used || !dst->bound)
                continue;
            if (dst->lport_be != dport_be)
                continue;
            if (dst->rx_count >= UDP_RXQ_DEPTH)
                break; /* drop */
            struct udp_rx_pkt *p = &dst->rxq[dst->rx_tail];
            p->src_be = net_local_ip_be();
            p->sport_be = s->lport_be;
            p->len = (u16)(len > UDP_RX_PAY ? UDP_RX_PAY : len);
            memcpy(p->data, data, p->len);
            dst->rx_tail = (u16)((dst->rx_tail + 1) % UDP_RXQ_DEPTH);
            dst->rx_count++;
            break;
        }
        /* Also attempt wire send for self (harmless if no listener outside). */
    }

    int r = net_udp_output(dst_be, s->lport_be, dport_be, data, len);
    if (r < 0)
        return r;
    return (int)len;
}

int net_sock_recvfrom(struct helix_sock *s, void *buf, u32 buflen,
                      u32 *src_be, u16 *sport_be)
{
    if (!s || !s->used || !buf)
        return -EINVAL;
    ensure_bound(s);
    if (s->rx_count == 0)
        return -EAGAIN;
    struct udp_rx_pkt *p = &s->rxq[s->rx_head];
    u32 n = p->len;
    if (n > buflen)
        n = buflen;
    memcpy(buf, p->data, n);
    if (src_be)
        *src_be = p->src_be;
    if (sport_be)
        *sport_be = p->sport_be;
    s->rx_head = (u16)((s->rx_head + 1) % UDP_RXQ_DEPTH);
    s->rx_count--;
    return (int)n;
}

/* Called from IPv4 RX path when UDP packet arrives for us. */
void net_udp_input(u32 src_be, u32 dst_be, u16 sport_be, u16 dport_be,
                   const u8 *payload, u32 plen)
{
    (void)dst_be;
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        struct helix_sock *s = &g_socks[i];
        if (!s->used || !s->bound)
            continue;
        if (s->lport_be != dport_be)
            continue;
        if (s->rx_count >= UDP_RXQ_DEPTH)
            return; /* drop */
        struct udp_rx_pkt *p = &s->rxq[s->rx_tail];
        p->src_be = src_be;
        p->sport_be = sport_be;
        p->len = (u16)(plen > UDP_RX_PAY ? UDP_RX_PAY : plen);
        if (p->len && payload)
            memcpy(p->data, payload, p->len);
        s->rx_tail = (u16)((s->rx_tail + 1) % UDP_RXQ_DEPTH);
        s->rx_count++;
        return;
    }
}
