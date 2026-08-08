#pragma once

#include "helix/types.h"

/* TCP protocol constants */
#define TCP_STATE_CLOSED      0
#define TCP_STATE_LISTEN      1
#define TCP_STATE_SYN_SENT    2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED 4
#define TCP_STATE_FIN_WAIT_1  5
#define TCP_STATE_FIN_WAIT_2  6
#define TCP_STATE_CLOSE_WAIT  7
#define TCP_STATE_LAST_ACK    8
#define TCP_STATE_TIME_WAIT   9

/* TCP header flags (RFC 793) */
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

/* Prototype TCP port (M14 — simple forward declaration) */
#define TCP_PORT_ANY    0x0000
#define TCP_CONN_BACKLOG 4   /* max pending connections */


/* TCP data packet queued for receive */
struct tcp_rx_seg {
    u32  seq;
    u32  len;
    u8   data[1400];   /* max payload */
};

/* TCP retransmission queue entry */
struct tcp_tx_seg {
    u32  seq;
    u32  len;
    u8   data[1400];
    u8   retries;
};

/* Forward-reference; actual definition in net.h only for kernel consumption */
struct helix_tcp_sock {
    int  used;
    int  state;             /* TCP_STATE_* */
    u32  laddr_be;          /* local address (network order) */
    u16  lport_be;          /* local port (network order); ephemeral if 0 */
    int  bound;

    /* Remote peer */
    u32  raddr_be;          /* remote address (network order) */
    u16  rport_be;          /* remote port (network order) */

    /* Sequence numbers */
    u32  snd_una;           /* oldest unacked byte */
    u32  snd_nxt;           /* next byte to send */
    u32  snd_wnd;           /* send window (peer's advertised window) */
    u32  rcv_nxt;           /* next expected byte from peer */
    u32  iss;               /* initial send sequence number */

    /* Receive queue */
    struct tcp_rx_seg rxq[8];
    u16  rx_head, rx_tail, rx_count;

    /* TX retransmission queue */
    struct tcp_tx_seg txq[16];
    u16  tx_head, tx_tail, tx_count;

    /* Accept backlog (for LISTEN sockets) */
    struct helix_tcp_sock *backlog[TCP_CONN_BACKLOG];
    int  backlog_len;

    /* Retransmission timer */
    u64  last_send_tick;      /* last time data was sent (for retransmit timer) */
    u32  retries;             /* SYN/FIN retransmit retry counter */

    /* Parent socket (for accepted connections) */
    struct helix_tcp_sock *parent;
};

/* TCP API (used by syscall.c, net.c) */
struct helix_tcp_sock *tcp_alloc_conn(void);
void  tcp_free(struct helix_tcp_sock *ts);
struct helix_tcp_sock *tcp_find_bound(u16 port_be);
int   tcp_bind(struct helix_tcp_sock *ts, u32 addr_be, u16 port_be);
int   tcp_connect(struct helix_tcp_sock *ts, u32 raddr_be, u16 rport_be);
int   tcp_listen(struct helix_tcp_sock *ts, u16 port_be);
struct helix_tcp_sock *tcp_accept(struct helix_tcp_sock *listen_sock);
int   tcp_send_data(struct helix_tcp_sock *ts, const void *data, u32 len);
int   tcp_recv_data(struct helix_tcp_sock *ts, void *buf, u32 buflen);
int   tcp_close(struct helix_tcp_sock *ts);
void  tcp_input(u32 src_be, u32 dst_be, const u8 *tcp_pkt, u32 tcp_len);
void  tcp_init(void);
void  tcp_retransmit(void);
