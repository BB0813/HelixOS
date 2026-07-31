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
    struct tcp_tx_seg txq[4];
    u16  tx_head, tx_tail, tx_count;

    /* Accept backlog (for LISTEN sockets) */
    struct helix_tcp_sock *backlog[TCP_CONN_BACKLOG];
    int  backlog_len;

    /* Parent socket (for accepted connections) */
    struct helix_tcp_sock *parent;
};
