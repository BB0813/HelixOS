/*
 * M14 TCP full stack — Ethernet + IPv4 + TCP state machine.
 *
 * Supported operations:
 *   - socket(AF_INET, SOCK_STREAM, 6) → create TCP socket
 *   - bind / listen / connect / accept
 *   - send (via sendto) / recv (via recvfrom) once ESTABLISHED
 *   - close (FIN handshake)
 *
 * Only client-side state machine implemented for M14 smoke:
 *   CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED
 */
#include "helix/tcp.h"
#include "helix/net.h"
#include "helix/kprintf.h"
#include "helix/string.h"
#include "helix/timer.h"
#include "helix/heap.h"

#define TCP_SOCK_MAX       8

/* Packed TCP header */
struct tcp_hdr {
    u16 sport_be;
    u16 dport_be;
    u32 seq;
    u32 ack;
    u8  data_offset; /* high nibble */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} __attribute__((packed));

/* Externals from net.c for building IP packets */
extern int  net_ready(void);
extern u32  net_local_ip_be(void);
extern int  ip_send(u32 dst_host, u8 proto, const void *payload, u32 plen);

static struct helix_tcp_sock g_tcp_socks[TCP_SOCK_MAX];
static u16 g_tcp_ephemeral = 41000; /* host order; next ephemeral port */

static u16 htons16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u16 ntohs16(u16 x) { return htons16(x); }
static u32 htons32(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static u32 ntohs32(u32 x) { return htons32(x); }

static u16 checksum16(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) { sum += ((u32)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* TCP pseudo-header for checksum (RFC 793) */
static u16 tcp_checksum(const void *tcp_segment, u32 tcp_len,
                        u32 src_be, u32 dst_be) {
    struct { u32 src, dst; u8 zero; u8 proto; u16 tcp_len; } __attribute__((packed)) ph;
    ph.src = src_be;
    ph.dst = dst_be;
    ph.zero = 0;
    ph.proto = 6; /* TCP */
    ph.tcp_len = htons16((u16)tcp_len);
    u32 sum = checksum16(&ph, sizeof(ph));
    sum += (u32)~checksum16(tcp_segment, tcp_len) & 0xFFFF; /* ones-complement */
    sum = (sum & 0xFFFF) + (sum >> 16);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* ---- allocation ---- */
struct helix_tcp_sock *tcp_alloc_conn(void) {
    for (int i = 0; i < TCP_SOCK_MAX; i++) {
        if (!g_tcp_socks[i].used) {
            struct helix_tcp_sock *ts = &g_tcp_socks[i];
            memset(ts, 0, sizeof(*ts));
            ts->used = 1;
            ts->state = TCP_STATE_CLOSED;
            ts->iss = (u32)(timer_ticks() * 1000 + i * 101); /* deterministic-ish */
            ts->snd_una = ts->iss;
            ts->snd_nxt = ts->iss + 1;
            return ts;
        }
    }
    return 0;
}

void tcp_free(struct helix_tcp_sock *ts) {
    if (!ts) return;
    memset(ts, 0, sizeof(*ts));
}

struct helix_tcp_sock *tcp_find_bound(u16 port_be) {
    for (int i = 0; i < TCP_SOCK_MAX; i++) {
        if (g_tcp_socks[i].used && g_tcp_socks[i].bound &&
            g_tcp_socks[i].lport_be == port_be)
            return &g_tcp_socks[i];
    }
    return 0;
}

/* ---- bind ---- */
int tcp_bind(struct helix_tcp_sock *ts, u32 addr_be, u16 port_be) {
    if (!ts || !ts->used) return -22;
    if (port_be == 0) {
        u16 p = g_tcp_ephemeral++;
        if (g_tcp_ephemeral < 41000) g_tcp_ephemeral = 41000;
        port_be = htons16(p);
    }
    if (tcp_find_bound(port_be)) return -98; /* EADDRINUSE */
    ts->laddr_be = addr_be;
    ts->lport_be = port_be;
    ts->bound = 1;
    return 0;
}

/* ---- helper: send TCP segment ---- */
static int tcp_send_segment(struct helix_tcp_sock *ts, u8 flags,
                            u32 seq, u32 ack, const void *data, u32 dlen) {
    if (!net_ready()) return -1;
    u8 buf[1500];
    u32 tcp_len = (u32)sizeof(struct tcp_hdr) + dlen;
    if (tcp_len > 1500 - 20) { dlen = 1500 - 20 - sizeof(struct tcp_hdr); tcp_len = (u32)sizeof(struct tcp_hdr) + dlen; }

    struct tcp_hdr *th = (struct tcp_hdr *)buf;
    memset(th, 0, sizeof(*th));
    th->sport_be = ts->lport_be;
    th->dport_be = ts->rport_be;
    th->seq = htons32(seq);
    th->ack = htons32(ack);
    th->data_offset = (u8)((sizeof(*th) / 4) << 4);
    th->flags = flags;
    th->window = htons16(4096);
    th->urgent = 0;
    if (dlen) memcpy(buf + sizeof(*th), data, dlen);
    th->checksum = htons16(tcp_checksum(buf, tcp_len, ts->laddr_be, ts->raddr_be));

    u32 dst_host = ((ts->raddr_be & 0xFF) << 24) | ((ts->raddr_be >> 8 & 0xFF) << 16) |
                   ((ts->raddr_be >> 16 & 0xFF) << 8) | (ts->raddr_be >> 24);
    return ip_send(dst_host, 6 /* TCP */, buf, tcp_len);
}

/* ---- connect (active open: CLOSED → SYN_SENT) ---- */
int tcp_connect(struct helix_tcp_sock *ts, u32 raddr_be, u16 rport_be) {
    if (!ts || !ts->used || ts->state != TCP_STATE_CLOSED) return -22;
    if (!ts->bound) tcp_bind(ts, 0, 0);
    ts->raddr_be = raddr_be;
    ts->rport_be = rport_be;
    ts->rcv_nxt = 0;
    ts->state = TCP_STATE_SYN_SENT;
    ts->snd_nxt = ts->iss;
    kprintf("[tcp] connect: sending SYN seq=%u to port %u\n", ts->iss, (unsigned)ntohs16(rport_be));
    return tcp_send_segment(ts, TCP_FLAG_SYN, ts->snd_nxt, 0, 0, 0);
}

/* ---- listen (passive open) ---- */
int tcp_listen(struct helix_tcp_sock *ts, u16 port_be) {
    if (!ts || !ts->used) return -22;
    if (!ts->bound) {
        ts->laddr_be = net_local_ip_be();
        ts->lport_be = port_be ? port_be : htons16(g_tcp_ephemeral++);
        ts->bound = 1;
    }
    ts->state = TCP_STATE_LISTEN;
    ts->backlog_len = 0;
    kprintf("[tcp] listening on port %u\n", (unsigned)ntohs16(ts->lport_be));
    return 0;
}

/* ---- accept: pick a connection from backlog ---- */
struct helix_tcp_sock *tcp_accept(struct helix_tcp_sock *listen_sock) {
    if (!listen_sock || listen_sock->state != TCP_STATE_LISTEN || listen_sock->backlog_len == 0)
        return 0;
    struct helix_tcp_sock *child = listen_sock->backlog[0];
    /* shift backlog */
    for (int i = 1; i < listen_sock->backlog_len; i++)
        listen_sock->backlog[i - 1] = listen_sock->backlog[i];
    listen_sock->backlog_len--;
    child->parent = 0;
    kprintf("[tcp] accept: new conn from port %u\n", (unsigned)ntohs16(child->rport_be));
    return child;
}

/* ---- send (data transfer in ESTABLISHED) ---- */
int tcp_send_data(struct helix_tcp_sock *ts, const void *data, u32 len) {
    if (!ts || ts->state != TCP_STATE_ESTABLISHED) return -22;
    if (len == 0) return 0;
    if (len > 1400) len = 1400;
    kprintf("[tcp] send %u bytes\n", (unsigned)len);
    return tcp_send_segment(ts, TCP_FLAG_PSH | TCP_FLAG_ACK, ts->snd_nxt, ts->rcv_nxt, data, len);
}

/* ---- recv (non-blocking; returns -EAGAIN if empty) ---- */
int tcp_recv_data(struct helix_tcp_sock *ts, void *buf, u32 buflen) {
    if (!ts || ts->state != TCP_STATE_ESTABLISHED) return -22;
    if (ts->rx_count == 0) return -11; /* EAGAIN */
    struct tcp_rx_seg *seg = &ts->rxq[ts->rx_head];
    u32 n = seg->len;
    if (n > buflen) n = buflen;
    memcpy(buf, seg->data, n);
    ts->rx_head = (u16)((ts->rx_head + 1) % 8);
    ts->rx_count--;
    ts->rcv_nxt += n;
    kprintf("[tcp] recv %u bytes (rcv_nxt now %u)\n", (unsigned)n, ts->rcv_nxt);
    return (int)n;
}

/* ---- close: FIN handshake ---- */
int tcp_close(struct helix_tcp_sock *ts) {
    if (!ts || !ts->used) return -9;
    if (ts->state == TCP_STATE_CLOSED) return -9;
    if (ts->state == TCP_STATE_ESTABLISHED) {
        ts->state = TCP_STATE_FIN_WAIT_1;
        kprintf("[tcp] close: sending FIN (FIN_WAIT_1)\n");
        return tcp_send_segment(ts, TCP_FLAG_FIN | TCP_FLAG_ACK, ts->snd_nxt, ts->rcv_nxt, 0, 0);
    }
    memset(ts, 0, sizeof(*ts));
    return 0;
}

/* ---- handle incoming TCP segment (from IPv4 RX path) ---- */
void tcp_input(u32 src_be, u32 dst_be_ignored, const u8 *tcp_pkt, u32 tcp_len) {
    (void)dst_be_ignored;
    if (!net_ready() || tcp_len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)tcp_pkt;
    u16 sport_be = th->sport_be;
    u16 dport_be = th->dport_be;
    u8  flags   = th->flags;
    u32 seq     = ntohs32(th->seq);
    u32 ack     = ntohs32(th->ack);
    u32 hdr_len = (u32)((th->data_offset >> 4) * 4);
    if (hdr_len < sizeof(*th) || hdr_len > tcp_len) return;

    const u8 *payload = tcp_pkt + hdr_len;
    u32 plen = tcp_len - hdr_len;

    kprintf("[tcp] input flags=0x%x seq=%u ack=%u len=%u sport=%u dport=%u\n",
            (unsigned)flags, seq, ack, (unsigned)tcp_len,
            (unsigned)ntohs16(sport_be), (unsigned)ntohs16(dport_be));

    /* Find matching socket */
    struct helix_tcp_sock *ts = 0;
    for (int i = 0; i < TCP_SOCK_MAX; i++) {
        struct helix_tcp_sock *s = &g_tcp_socks[i];
        if (!s->used) continue;
        if (s->state == TCP_STATE_LISTEN && s->bound && s->lport_be == dport_be) {
            ts = s; /* matched listener; will accept below */
        } else if (s->state != TCP_STATE_LISTEN && s->state != TCP_STATE_CLOSED &&
                   s->lport_be == dport_be && s->rport_be == sport_be &&
                   s->raddr_be == src_be) {
            ts = s; /* matched established / connecting */
            break;
        }
    }

    if (!ts) { /* no socket — send RST */
        kprintf("[tcp] no listener for port %u\n", (unsigned)ntohs16(dport_be));
        return;
    }

    /* Passive open: LISTEN + SYN → create child, SYN+ACK */
    if (ts->state == TCP_STATE_LISTEN && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        if (ts->backlog_len >= TCP_CONN_BACKLOG) {
            kprintf("[tcp] backlog full\n");
            return;
        }
        struct helix_tcp_sock *child = tcp_alloc_conn();
        if (!child) return;
        child->laddr_be = ts->laddr_be;
        child->lport_be = ts->lport_be;
        child->raddr_be = src_be;
        child->rport_be = sport_be;
        child->bound = 1;
        child->state = TCP_STATE_SYN_RECEIVED;
        child->rcv_nxt = seq + 1;
        child->iss = (u32)(timer_ticks() * 997);
        child->snd_nxt = child->iss;
        child->snd_una = child->iss;
        child->parent = ts;
        kprintf("[tcp] SYN_RECEIVED: sending SYN+ACK seq=%u ack=%u\n", child->iss, child->rcv_nxt);
        tcp_send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, child->snd_nxt, child->rcv_nxt, 0, 0);
        return;
    }

    /* SYN_RECEIVED + ACK → ESTABLISHED, add to backlog */
    if (ts->state == TCP_STATE_SYN_RECEIVED && (flags & TCP_FLAG_ACK)) {
        ts->state = TCP_STATE_ESTABLISHED;
        ts->rcv_nxt = seq; /* peer may send data starting at seq */
        ts->snd_una = ack;
        ts->snd_nxt = ack;
        struct helix_tcp_sock *parent = ts->parent;
        if (parent && parent->backlog_len < TCP_CONN_BACKLOG) {
            parent->backlog[parent->backlog_len++] = ts;
            kprintf("[tcp] ESTABLISHED: added to listen backlog (len=%d)\n", parent->backlog_len);
        }
        return;
    }

    /* SYN_SENT + SYN+ACK → ESTABLISHED (active open) */
    if (ts->state == TCP_STATE_SYN_SENT && (flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
        ts->state = TCP_STATE_ESTABLISHED;
        ts->rcv_nxt = seq + 1;
        ts->snd_una = ack;
        ts->snd_nxt = ack;
        kprintf("[tcp] ESTABLISHED: active connection completed, sending ACK\n");
        tcp_send_segment(ts, TCP_FLAG_ACK, ts->snd_nxt, ts->rcv_nxt, 0, 0);
        return;
    }

    /* ESTABLISHED: data or ACK or FIN */
    if (ts->state == TCP_STATE_ESTABLISHED) {
        /* Incoming data */
        if (plen > 0 && ts->rx_count < 8) {
            struct tcp_rx_seg *seg = &ts->rxq[ts->rx_tail];
            seg->seq = seq;
            seg->len = plen;
            if (plen > sizeof(seg->data)) plen = sizeof(seg->data);
            memcpy(seg->data, payload, plen);
            seg->len = plen;
            ts->rx_tail = (u16)((ts->rx_tail + 1) % 8);
            ts->rx_count++;
            /* Send ACK */
            tcp_send_segment(ts, TCP_FLAG_ACK, ts->snd_nxt, seq + plen, 0, 0);
        }
        /* Remote FIN → our CLOSE_WAIT */
        if (flags & TCP_FLAG_FIN) {
            ts->state = TCP_STATE_CLOSE_WAIT;
            kprintf("[tcp] CLOSE_WAIT: remote sent FIN, send ACK\n");
            tcp_send_segment(ts, TCP_FLAG_ACK, ts->snd_nxt, seq + 1, 0, 0);
        }
        return;
    }

    /* FIN_WAIT_1 + ACK → FIN_WAIT_2 */
    if (ts->state == TCP_STATE_FIN_WAIT_1 && (flags & TCP_FLAG_ACK)) {
        if (flags & TCP_FLAG_FIN) {
            ts->state = TCP_STATE_TIME_WAIT;
            kprintf("[tcp] TIME_WAIT: received FIN+ACK\n");
            tcp_send_segment(ts, TCP_FLAG_ACK, ts->snd_nxt, seq + 1, 0, 0);
            /* After TIME_WAIT, free socket */
            memset(ts, 0, sizeof(*ts));
        } else {
            ts->state = TCP_STATE_FIN_WAIT_2;
            kprintf("[tcp] FIN_WAIT_2: waiting for remote FIN\n");
        }
        return;
    }

    /* FIN_WAIT_2 + FIN → TIME_WAIT → CLOSED */
    if (ts->state == TCP_STATE_FIN_WAIT_2 && (flags & TCP_FLAG_FIN)) {
        ts->state = TCP_STATE_TIME_WAIT;
        kprintf("[tcp] TIME_WAIT: received FIN\n");
        tcp_send_segment(ts, TCP_FLAG_ACK, ts->snd_nxt, seq + 1, 0, 0);
        memset(ts, 0, sizeof(*ts));
        return;
    }
}

/* ---- helper: add known ports for dedicated listeners ---- */
void tcp_init(void)
{
    memset(g_tcp_socks, 0, sizeof(g_tcp_socks));
    g_tcp_ephemeral = 41000;
    kprintf("[tcp] M14 TCP module ready\n");
}
