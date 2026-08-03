# Goal: HelixOS M15 — 用户态 TCP 完善

> **状态：进行中** — **`HelixTcpUserOK`**（待验证）

## 范围

1. helixbox TCP echo 自检：`socket(SOCK_STREAM)` + `connect(10.0.2.2:8080)` + `sendto`/`recvfrom` → **`HelixTcpUserOK`**
2. `sendto`/`recvfrom` 路由：`is_socket==1` → UDP；`is_socket==2` → TCP（`tcp_send_data`/`tcp_recv_data`）
3. QEMU hostfwd TCP：`-netdev user,...,hostfwd=tcp::8080-:8080`
4. host echo server：`scripts/tcp_echo_server.py`（仅 smoke-net 期间启动）

## 约束 / 非目标

- 协作调度：TCP 连接建立/收发用 EAGAIN + 用户态 yield 轮询
- TCP 无重传；QEMU user net 本地路径 SYN 丢包概率极低
- host echo server 仅用于 smoke 测试，不作为常驻服务
- `sendmsg`/`recvmsg` 完整映射留后续

## 验收

```bash
make smoke-net
# serial.log: HelixTcpUserOK + HelixTcpOK + HelixNetOK + user_udp_ok
```

## 实现要点

- `kernel/proc/syscall.c`：`sys_sendto` / `sys_recvfrom` 增加 `is_socket==2` 路由到 TCP
- `user/helixbox.c`：`cmd_tcp_smoke()` — socket(SOCK_STREAM) → fcntl(O_NONBLOCK) → connect → poll send → poll recv → `HelixTcpUserOK`
- `scripts/run-qemu.sh`：netdev 增加 `hostfwd=tcp::8080-:8080`
- `scripts/tcp_echo_server.py`：Python TCP echo server，前缀 `ECHO:` 回显
- `Makefile` smoke-net：启动/终止 tcp_echo_server.py
