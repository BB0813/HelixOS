# Goal: HelixOS M7 — 网络与图形（可后置）

## 项目
HelixOS：自研 x86_64 内核 Helix + Linux 用户态兼容（非 Linux 源码/发行版）。
UEFI only。M0–M6 已完成：启动/核/shell/Ring3；AHCI+GPT+FAT 读写 + `/tmp` ramfs；Linux 号 syscall 子集 + helixbox/BusyBox echo；**最小动态** `ld-helix` + **真 musl** `ld-musl`/`hello.musl`（`HelloDynOK` / `HelloMuslDynOK`）。
**尚无**：网卡驱动、协议栈、用户态 socket、framebuffer GUI。

QEMU 现状：`q35` + AHCI 盘 + 串口；**无** `-netdev`/`virtio-net` 挂载。主路径仍 Windows/MSYS 编内核；网络测资可用 NAS/Linux 交叉。

## 本目标（一次做完，可构建可验证）
在 M6 之上建立 **最小可用网络路径**（图形可选、不挡 CLI）：

1. **网卡驱动（QEMU 友好）**  
   - 优先 **virtio-net-pci**（现代、文档多）；备选 e1000 若更简单  
   - PCI 发现 + MMIO/virtqueue 最小收发；同步或轮询即可（可不做中断完善）  
   - 接口形状示例：`net_init` / `net_send` / `net_recv`；单设备  
   - `scripts/run-qemu.sh` 增加 netdev（user/`-netdev user` 或 tap；Windows 上优先 **user 模式** 免装 tap）

2. **最小协议**  
   - 至少：**以太网帧** 收发 + **ARP** 应答本机 MAC/IP（可静态配置）  
   - **IPv4** 收发包（固定 IP，如 `10.0.2.15/24` 对 QEMU user net）  
   - **ICMP echo reply**（ping 通）**或** **UDP echo** 二选一作为主验收（推荐 ICMP：`ping` 从宿主机/user net）  
   - TCP 可 stub/`-ENOSYS`；完整 TCP 不要求本里程碑一次做完

3. **用户态可见性（Linux 号优先）**  
   - 最小 socket 子集（能支撑验收即可），例如：  
     - `socket` / `bind` / `sendto` / `recvfrom`（UDP）或内核自测 ICMP 不暴露 socket  
   - 若本阶段只做 **内核态 ping 响应** + 串口日志，须在文档标明；**优先**至少一条用户态路径（helixbox 小 applet 或 freestanding `ping`/`udpecho`）  
   - 未知号继续 `-ENOSYS`；写入 `SYSCALLS.md`

4. **构建与 smoke**  
   - 新增 **`make smoke-net`**（名可改）：  
     - 串口出现网卡 ready / IP 配置日志  
     - 证明收包或发包（如 `ICMP echo reply` 计数、或 UDP 回显字符串 `HelixNetOK`）  
   - 既有 `smoke` / `smoke-fs` / `smoke-dyn` / `smoke-musl` / `smoke-linux` **不破**  
   - Windows/QEMU **user networking** 步骤写进 `BUILD.md`（端口转发若需要）

5. **图形（可选加分，非必须）**  
   - GOP/virtio-gpu/简单 framebuffer 清屏 + 字符或矩形  
   - **不得**阻塞网络主验收；可单独 `smoke-fb` 或文档手测

6. **文档**  
   - `ROADMAP` 勾选 M7 已做项；`ARCHITECTURE` 网络栈边界；`BUILD` QEMU 网络参数；`SYSCALLS` 新增 socket 类；`README` 状态  
   - 诚实：非完整 Linux 网络栈、无路由协议、无 TLS

## 约束
- C + 极少 asm；现有 freestanding 链；串口仍是主日志  
- 不引入 Linux **内核**源码；可参考规范/公开文档  
- 中断上下文不做重阻塞；收包可轮询于 idle/syscall  
- 少空谈；简体中文 + 英文标识符  
- 第三方（若有）放 `third_party/` 并注明许可

## 验收
- `make && make smoke-net` 通过（约定串口标记，如 `HelixNetOK` 或 ping 相关明确日志）  
- 回归：`make smoke-dyn` / `make smoke-musl` 仍绿（或文档说明短暂跳过原因）  
- QEMU 命令可复制；`SYSCALLS.md` 含本阶段网络相关项  
- 若仅内核 ICMP：宿主机 `ping` 通 guest（user net 下按 BUILD 说明）须可复现

## 非目标
- 完整 TCP（HTTP 服务器可后置）  
- DHCP 客户端完备（可静态 IP）  
- IPv6、防火墙、多网卡、Wi‑Fi  
- 桌面 GUI / Wayland / GPU 加速  
- 替换 M6 musl 加载器

## 建议顺序
1. QEMU 挂 virtio-net + PCI 探测 + 收发包环  
2. 以太网 + ARP + IPv4 + ICMP 或 UDP  
3. 可选 socket syscall + 用户态小测  
4. `smoke-net` + 回归 + 文档  

## 开工
先读 `scripts/run-qemu.sh`、PCI/AHCI 驱动模式、`idle` 循环与现有 smoke；网络以 **可 ping 或 UDP 回显** 为第一绿。阶段末给验证命令与 `serial.log` 摘录。
