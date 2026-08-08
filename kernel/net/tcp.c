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

/* M17 retransmission constants */
#define TCP_RETRANS_TIMEOUT_TICKS  50   /* 0.5 second at 100Hz — fast SYN retransmit */
#define TCP_RETRANS_MAX_RETRIES    3

#define TCP_SOCK_MAX       8
#define TCP_PENDING_MAX    4  /* SYNReceived children waiting for a listener */

struct tcp_pending_child {
    struct helix_tcp_sock *sock;
    int has_ack;        /* ACK received before listener existed */
    u32 ack_seq;        /* ACK's seq value */
    int data_len;       /* data received before listener existed */
    u8  data[1400];     /* buffered data */
};

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
static struct tcp_pending_child g_tcp_pending[TCP_PENDING_MAX];
static int g_tcp_pending_count;
static u16 g_tcp_ephemeral = 41000; /* host order; next ephemeral port */

static u16 htons16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u16 ntohs16(u16 x) { return htons16(x); }
static u32 htons32(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static u32 ntohs32(u32 x) { return htons32(x); }

static u16 checksum16_raw(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) { sum += ((u32)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)sum;
}

/* TCP pseudo-header for checksum (RFC 793)
 * src_be/dst_be are stored in "wire byte order as u32 on little-endian host":
 * i.e. the LSB is the first byte on the wire (matches how sys_connect parses
 * sockaddr bytes). So byte 0 of the pseudo-header = LSB of the u32. */
static u16 tcp_checksum(const void *tcp_segment, u32 tcp_len,
                        u32 src_be, u32 dst_be) {
    u8 ph[12];
    ph[0] = (u8)src_be;
    ph[1] = (u8)(src_be >> 8);
    ph[2] = (u8)(src_be >> 16);
    ph[3] = (u8)(src_be >> 24);
    ph[4] = (u8)dst_be;
    ph[5] = (u8)(dst_be >> 8);
    ph[6] = (u8)(dst_be >> 16);
    ph[7] = (u8)(dst_be >> 24);
    ph[8] = 0;
    ph[9] = 6; /* TCP */
    ph[10] = (u8)(tcp_len >> 8);
    ph[11] = (u8)tcp_len;
    u32 sum = checksum16_raw(ph, sizeof(ph));
    sum += checksum16_raw(tcp_segment, tcp_len);
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
    /* Default to local IP if user passed INADDR_ANY (0) — pseudo-header needs
     * the same src that goes into the IP header. */
    if (addr_be == 0) addr_be = net_local_ip_be();
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
    /* raddr_be and laddr_be are both in network order; tcp_checksum reads bytes as-is */
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
    ts->retries = 0;
    ts->last_send_tick = timer_ticks();
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

    /* M17: adopt any pending SYN_RECEIVED children for this port */
    for (int i = 0; i < g_tcp_pending_count; i++) {
        struct tcp_pending_child *pc = &g_tcp_pending[i];
        if (pc->sock && pc->sock->lport_be == ts->lport_be && ts->backlog_len < TCP_CONN_BACKLOG) {
            struct helix_tcp_sock *child = pc->sock;
            child->parent = ts;
            if (pc->has_ack) {
                /* Client ACK was received before listener — transition to ESTABLISHED */
                child->state = TCP_STATE_ESTABLISHED;
                child->rcv_nxt = pc->ack_seq;
                child->snd_una = pc->ack_seq;
                child->snd_nxt = pc->ack_seq;
            }
            /* Deliver any data that arrived before the listener existed */
            if (pc->data_len > 0 && child->rx_count < 8) {
                struct tcp_rx_seg *seg = &child->rxq[child->rx_tail];
                seg->seq = child->rcv_nxt;
                seg->len = (u32)pc->data_len;
                memcpy(seg->data, pc->data, (u32)pc->data_len);
                child->rx_tail = (u16)((child->rx_tail + 1) % 8);
                child->rx_count++;
                child->rcv_nxt += (u32)pc->data_len;
                kprintf("[tcp] adopted: delivered %d pending bytes (rx_count=%d)\n", pc->data_len, child->rx_count);
            }
            ts->backlog[ts->backlog_len++] = child;
            kprintf("[tcp] adopted pending child from port %u into backlog (state=%d, rx_count=%d)\n",
                    (unsigned)ntohs16(child->rport_be), child->state, child->rx_count);
        }
    }
    /* compact pending array */
    {
        int w = 0;
        for (int i = 0; i < g_tcp_pending_count; i++) {
            if (g_tcp_pending[i].sock && g_tcp_pending[i].sock->lport_be != ts->lport_be)
                g_tcp_pending[w++] = g_tcp_pending[i];
        }
        g_tcp_pending_count = w;
    }

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

    /* M17: queue to TXQ before sending */
    if (ts->tx_count < 16) {
        struct tcp_tx_seg *tx = &ts->txq[ts->tx_tail];
        tx->seq = ts->snd_nxt;
        tx->len = len;
        memcpy(tx->data, data, len);
        tx->retries = 0;
        ts->tx_tail = (ts->tx_tail + 1) % 16;
        ts->tx_count++;
    }

    kprintf("[tcp] send %u bytes\n", (unsigned)len);
    ts->last_send_tick = timer_ticks();
    int ret = tcp_send_segment(ts, TCP_FLAG_PSH | TCP_FLAG_ACK, ts->snd_nxt, ts->rcv_nxt, data, len);
    ts->snd_nxt += len;
    return ret;
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

    if (!ts) { /* no socket — hold as pending child, or deliver to existing pending child */
        /* Check if this packet belongs to an existing pending child */
        for (int i = 0; i < g_tcp_pending_count; i++) {
            struct tcp_pending_child *pc = &g_tcp_pending[i];
            if (pc->sock && pc->sock->rport_be == sport_be && pc->sock->lport_be == dport_be) {
                if ((flags & TCP_FLAG_ACK) && !(flags & TCP_FLAG_SYN)) {
                    pc->has_ack = 1;
                    pc->ack_seq = seq;
                    kprintf("[tcp] PENDING: ACK received, seq=%u\n", seq);
                }
                if (plen > 0 && pc->data_len == 0 && plen <= sizeof(pc->data)) {
                    memcpy(pc->data, payload, plen);
                    pc->data_len = (int)plen;
                    kprintf("[tcp] PENDING: data received (%u bytes)\n", (unsigned)plen);
                }
                return;
            }
        }
        /* New SYN — create pending child */
        if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK) && g_tcp_pending_count < TCP_PENDING_MAX) {
            struct helix_tcp_sock *child = tcp_alloc_conn();
            if (child) {
                child->laddr_be = net_local_ip_be();
                child->lport_be = dport_be;
                child->raddr_be = src_be;
                child->rport_be = sport_be;
                child->bound = 0;
                child->state = TCP_STATE_SYN_RECEIVED;
                child->rcv_nxt = seq + 1;
                child->iss = (u32)(timer_ticks() * 997);
                child->snd_nxt = child->iss;
                child->snd_una = child->iss;
                child->parent = 0;
                struct tcp_pending_child *pc = &g_tcp_pending[g_tcp_pending_count++];
                pc->sock = child;
                pc->has_ack = 0;
                pc->data_len = 0;
                kprintf("[tcp] PENDING SYN from port %u\n", (unsigned)ntohs16(sport_be));
                tcp_send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, child->snd_nxt, child->rcv_nxt, 0, 0);
            }
        }
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
        /* If this is a pending child (no listener yet) and the ACK carries data,
         * stash the payload in the pending_child slot so listen() can deliver it.
         * Also mark has_ack so listen() will adopt it as ESTABLISHED. */
        if (!ts->parent) {
            for (int i = 0; i < g_tcp_pending_count; i++) {
                struct tcp_pending_child *pc = &g_tcp_pending[i];
                if (pc->sock == ts) {
                    pc->has_ack = 1;
                    pc->ack_seq = seq;
                    if (plen > 0 && pc->data_len == 0 && plen <= (u32)sizeof(pc->data)) {
                        memcpy(pc->data, payload, plen);
                        pc->data_len = (int)plen;
                    }
                    break;
                }
            }
        }
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
        /* M17: ACK — advance snd_una, drain confirmed TXQ entries */
        if (flags & TCP_FLAG_ACK) {
            if (ack > ts->snd_una)
                ts->snd_una = ack;
            while (ts->tx_count > 0) {
                struct tcp_tx_seg *oldest = &ts->txq[ts->tx_head];
                if (oldest->seq + oldest->len <= ts->snd_una) {
                    ts->tx_head = (ts->tx_head + 1) % 16;
                    ts->tx_count--;
                } else break;
            }
        }
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

/* ---- M17: retransmit timed-out segments (SYN, data, FIN) ---- */
void tcp_retransmit(void) {
    u64 now = timer_ticks();
    for (int i = 0; i < TCP_SOCK_MAX; i++) {
        struct helix_tcp_sock *ts = &g_tcp_socks[i];
        if (!ts->used) continue;

        /* SYN_SENT: retransmit SYN every 1s if no SYN+ACK received */
        if (ts->state == TCP_STATE_SYN_SENT) {
            if (now - ts->last_send_tick < TCP_RETRANS_TIMEOUT_TICKS) continue;
            if (ts->retries >= TCP_RETRANS_MAX_RETRIES) {
                /* D1: rate-limit max-retries log to 1 per 30s so serial.log stays
                 * greppable. Application can still poll the socket; tcpstat shows state. */
                static u64 s_last_retransmit_log = 0;
                u64 now_log = timer_ticks();
                if (now_log - s_last_retransmit_log < 3000) continue;  /* 30s @100Hz */
                s_last_retransmit_log = now_log;
                kprintf("[tcp] SYN retransmit: max retries reached (socket %d still waiting)\n", i);
                continue;
            }
            kprintf("[tcp] SYN retransmit: socket %d (retry %u)\n", i, (unsigned)ts->retries + 1);
            ts->retries++;
            ts->last_send_tick = now;
            tcp_send_segment(ts, TCP_FLAG_SYN, ts->snd_nxt, 0, 0, 0);
            continue;
        }

        if (ts->state != TCP_STATE_ESTABLISHED && ts->state != TCP_STATE_FIN_WAIT_1)
            continue;
        if (ts->tx_count == 0) continue;
        if (now - ts->last_send_tick < TCP_RETRANS_TIMEOUT_TICKS) continue;

        /* Resend all unacked segments. Iterate by count (not head != tail) so a
         * full ring (tx_count == 16, head == tail) still retransmits everything. */
        kprintf("[tcp] retransmit socket %d (tx_count=%u)\n", i, (unsigned)ts->tx_count);
        int dropped = 0;
        for (u16 k = 0; k < ts->tx_count; k++) {
            u16 idx = (u16)((ts->tx_head + k) % 16);
            struct tcp_tx_seg *tx = &ts->txq[idx];
            tx->retries++;
            if (tx->retries > TCP_RETRANS_MAX_RETRIES) {
                kprintf("[tcp] retransmit: max retries exceeded, closing socket %d\n", i);
                memset(ts, 0, sizeof(*ts));
                dropped = 1;
                break;
            }
            kprintf("[tcp] retransmit: resend seq=%u len=%u retries=%u\n",
                    tx->seq, tx->len, (unsigned)tx->retries);
            tcp_send_segment(ts, TCP_FLAG_PSH | TCP_FLAG_ACK, tx->seq, ts->rcv_nxt, tx->data, tx->len);
        }
        if (!dropped)
            ts->last_send_tick = now;
    }
}

/* ---- helper: add known ports for dedicated listeners ---- */
void tcp_init(void)
{
    /* M18: only clear genuinely UNUSED slots. Anything `used` (including pending
     * children created by incoming SYNs that arrived before userland bring-up)
     * must be preserved — otherwise tcp_init wipes the pending child slot and
     * the next socket() reuses it for the active connection. By the time listen()
     * runs, pc->sock points at the active socket instead of the pending child,
     * and the adoption check `pc->sock->lport_be == ts->lport_be` fails because
     * the active socket's lport is 41000 (ephemeral), not 8081. */
    for (int i = 0; i < TCP_SOCK_MAX; i++) {
        if (g_tcp_socks[i].used)
            continue;
        memset(&g_tcp_socks[i], 0, sizeof(g_tcp_socks[i]));
    }
    g_tcp_ephemeral = 41000;
    kprintf("[tcp] M14 TCP module ready (socks:");
    for (int i = 0; i < TCP_SOCK_MAX; i++)
        if (g_tcp_socks[i].used) kprintf(" %d:state=%d", i, g_tcp_socks[i].state);
    kprintf(")\n");
}
