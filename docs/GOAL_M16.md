# Goal: HelixOS M16 — sendmsg/recvmsg + TCP passive hostfwd

> **状态：已完成（2026-08-01）** — **`HelixTcpPassiveOK`**。

## 范围

1. `sendmsg`(46)：iovec coalesce → 单 kernel buffer → TCP/UDP send
2. `recvmsg`(47)：TCP/UDP recv → kernel buffer → iovec scatter + msg_name 填充
3. helixbox TCP 被动 smoke：listen(8081) + accept + recv + echo → **`HelixTcpPassiveOK`**
4. QEMU `hostfwd=tcp::8081-:8081`；host client `tcp_host_client.py`

## 约束 / 非目标

- 协作调度：listen/accept/recv 用 EAGAIN + yield 轮询
- iovec coalesce 限 4096 字节（足够典型 use case）
- ancillary data（msg_control）未处理（静默忽略）
- TCP 无重传（TXQ 保留但未驱动）— 留 M17

## 验收

```bash
make smoke-net
# serial.log: HelixTcpPassiveOK + HelixTcpUserOK + HelixTcpOK + HelixNetOK + user_udp_ok
```

## 实现要点

- `kernel/proc/syscall.c`：`sys_sendmsg` / `sys_recvmsg`（替换 ENOSYS stubs）
- `user/helixbox.c`：`cmd_tcp_passive_smoke()` — socket → bind → listen → accept poll → read poll → write ECHO:prefix+data
- `scripts/tcp_host_client.py`：host-side TCP client，connect + send + recv verify
- `scripts/run-qemu.sh`：netdev 增加 `hostfwd=tcp::8081-:8081`
- `Makefile` smoke-net：启动 tcp_host_client.py + tcp_echo_server.py
