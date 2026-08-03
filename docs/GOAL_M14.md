# Goal: HelixOS M14 — TCP 全栈（历史）

> **状态：已完成（2026-08-01）** — **`HelixTcpOK`**。

## 范围

1. TCP 状态机（RFC 793 极简）：CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT(1/2) → TIME_WAIT → CLOSED；LISTEN/SYN_RECEIVED/CLOSE_WAIT/LAST_ACK
2. `socket`(SOCK_STREAM) + `connect`/`listen`/`accept`：`is_socket=2` 标记 + `helix_tcp_sock` 分配
3. TCP RXQ(8) / TXQ(4) per-socket 队列
4. `sendto` / `recvfrom` 路由：TCP(PSH+ACK) / UDP 共路径
5. 内核自测：ICMP gate ping 后 `tcp_init` → **`HelixTcpOK`**
6. QEMU user net 无 host 侧 TCP 服务；用户态测试留待 M15

## 约束 / 非目标

- 协作调度下 TCP 阻塞操作用 EAGAIN + 用户态 yield 轮询
- 无滑动窗口/拥塞控制/重传（TXQ 保留但未接驱动）
- 无 Nagle 算法/延迟 ACK
- QEMU user net `hostfwd=tcp::` 不支持 SO_REUSEADDR 语义
- 用户态 TCP echo/ping 留 M15

## 验收

```bash
make smoke-net
# serial.log: HelixTcpOK + HelixNetOK + user_udp_ok
```

## 实现要点

- `include/helix/tcp.h`：状态常量、flag 常量、`tcp_rx_seg`、`tcp_tx_seg`、`helix_tcp_sock`
- `kernel/net/tcp.c`（~380行）：tcp_alloc_conn / tcp_bind / tcp_connect / tcp_listen / tcp_accept / tcp_send_data / tcp_recv_data / tcp_close / tcp_input / tcp_init
- `kernel/net/net.c`：`ip_send` 改为非 static；IPv4 demux 增加 `IP_PROTO_TCP` → `tcp_input()`
- `kernel/proc/syscall.c`：`sys_socket` SOCK_STREAM → `tcp_alloc_conn()`，`is_socket=2`；`sys_bind`/`sys_connect`/`sys_accept`/`sys_listen` 路由 TCP
