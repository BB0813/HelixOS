# Goal: HelixOS M17 — TCP retransmission (TXQ 驱动)

> **状态：已完成（2026-08-03）**

## 范围

1. **TXQ 入队**：`tcp_send_data()` 数据段发送前入 TXQ 环形队列
2. **ACK 清除**：`tcp_input()` ESTABLISHED 状态收到 ACK 时清除已确认段 + 推进 `snd_una`
3. **重传定时器**：`tcp_retransmit()` 检查 1 秒超时，重发所有未确认段，超过3次重试则关闭 socket
4. **基础设施修复**：tcp.h TCP API 声明、`__chkstk` stub、`tcp_init()` 保留 active listener

## 实现要点

- `include/helix/tcp.h`：`helix_tcp_sock` 增加 `last_send_tick`；TCP API 函数声明
- `kernel/net/tcp.c`：
  - `tcp_send_data()` — TXQ 入队 + `snd_nxt += len`
  - `tcp_input()` — ESTABLISHED ACK 推进 `snd_una`，drain TXQ
  - `tcp_retransmit()` — 新函数，从 `net_poll()` 调用
  - `tcp_init()` — 只清理 unbound socket，保留 active listener
- `kernel/net/net.c`：`net_poll()` 末尾调用 `tcp_retransmit()`
- `libk/chkstk.S` — `__chkstk` no-op stub（修复 Windows PE 链接）
- `user/helixbox.c`：`SYS_listen` 参数修复（3→4 args）

## 约束 / 非目标

- 协作调度下重传依赖 `net_poll()` 周期调用
- 超时阈值硬编码 1 秒（100 ticks @ 100Hz）；未动态调整
- QEMU user net hostfwd TCP：guest→host 连接（M15 active test）依赖 SLiRP NAT 直通；hostfwd 规则仅 host→guest 方向生效
- TCP passive smoke（M16 HelixTcpPassiveOK）依赖 host client 脚本在 Windows MSYS2 下正确建立连接，存在时序竞态

## 验收

```bash
make smoke-net
# serial.log: HelixNetOK + user_udp_ok + HelixTcpOK + SMOKE-NET OK
```
