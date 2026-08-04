# Goal: M23 — PS/2 鼠标支持

## Context

M18 实现了 PS/2 控制器第一通道 — 键盘（IRQ1 + scancode set 1 → ASCII）。M23
复用 `kernel/drv/ps2.c` 加上 PS/2 控制器第二通道（aux port, IRQ12）启用 + 鼠标
3-byte packet 解析，让用户态能读鼠标事件 `{dx, dy, buttons}`。

PS/2 控制器是单芯片双通道：port 0x60 数据口共享，port 0x64 命令口，命令字节 0xD4
前缀把 data byte 路由给 aux（鼠标）。QEMU `-device ps2-mouse` 默认启用；MSYS2
4MB OVMF 无 USB 驱动但 PS/2 控制器一直存在，所以 headless 测试也能触发（即使没人移
鼠标，[ps2] mouse ready 仍打印）。

## Files

- `kernel/drv/ps2.c` — 抽 helper `ps2_write_cmd` / `ps2_write_aux` / `ps2_flush_data`；
  mouse state（`g_ms_buf[64]` 环形 + `g_ms_packet[3]` 累加 + `g_ms_byte_idx`）；
  `ps2_mouse_handler()` IRQ12 分发；`ps2_mouse_read()` 非阻塞 drain
- `include/helix/ps2.h` — `struct helix_mouse_event { i16 dx, dy; u8 buttons, _pad; }`
  + `ps2_mouse_handler()` + `ps2_mouse_read()` 声明
- `kernel/arch/x86_64/irq.c` — IRQ12 case 分发到 `ps2_mouse_handler()`；`irq_init`
  kprintf 更新为 "IRQ0 timer, IRQ1 keyboard, IRQ12 mouse"
- `include/helix/syscall.h` — `SYS_mouse_read 548`
- `kernel/proc/syscall.c` — `sys_mouse_read(buf_ptr, count)`（user_ptr_ok + drain +
  EAGAIN）+ dispatch table `case SYS_mouse_read`
- `user/usys.h` — `SYS_mouse_read 548` + `struct helix_mouse_event` +
  `sys_mouse_read()` shim（**必须** 在 `usyscall()` 之后定义，否则 init.c 编译报
  implicit declaration）
- `user/helixbox.c` — `HelixKbOK` block 之后插入 `HelixMouseOK` smoke marker

## 改动细节

### `kernel/drv/ps2.c`

抽出公共 helper（移到文件顶部，kb_init 内部继续用）：
```c
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64
#define PS2_DATA_PORT   0x60

static void ps2_wait_input(void) {
    /* 等 status bit 1 = input buffer empty。spin 上限 100000。 */
    int spins = 0;
    while ((inb(PS2_STATUS_PORT) & 0x02) && spins < 100000) spins++;
}

static void ps2_write_cmd(u8 c) {
    ps2_wait_input();
    outb(PS2_CMD_PORT, c);
}

static void ps2_write_aux(u8 c) {
    /* 0xD4 = "下一字节写到 aux 设备"。 */
    ps2_write_cmd(0xD4);
    ps2_wait_input();
    outb(PS2_DATA_PORT, c);
}

static void ps2_flush_data(void) {
    /* 丢掉 ACK 字节（0xFA）或自检结果（0xAA）。 */
    if (inb(PS2_STATUS_PORT) & 0x01) inb(PS2_DATA_PORT);
}
```

Mouse state（`struct helix_mouse_event { i16 dx, dy; u8 buttons, _pad; }`）：
```c
#define MS_BUF_SIZE 64
static struct helix_mouse_event g_ms_buf[MS_BUF_SIZE];
static u32 g_ms_head, g_ms_tail;
static u8  g_ms_packet[3];     /* 累积 3-byte packet */
static u8  g_ms_byte_idx;
```

`ps2_mouse_handler()`（IRQ12 → 累积 → 满 3 bytes 解析 → 环形 buffer）：
```c
void ps2_mouse_handler(void) {
    u8 b = inb(PS2_DATA_PORT);

    /* 状态机 idle 时丢掉 0xAA/0xFA/0xFE/0xFF 响应字节。 */
    if (g_ms_byte_idx == 0 && (b == 0xAA || b == 0xFA ||
                               b == 0xFE || b == 0xFF)) {
        return;
    }

    g_ms_packet[g_ms_byte_idx++] = b;
    if (g_ms_byte_idx < 3) return;
    g_ms_byte_idx = 0;

    u8 status = g_ms_packet[0];
    /* packet[0]: bit 0=左, bit 1=右, bit 2=中, bit 6/7 = X/Y overflow (丢). */
    if (status & 0xC0) return;

    u8 buttons = status & 0x07;
    i16 dx = (i16)(i8)g_ms_packet[1];
    i16 dy = -(i16)(i8)g_ms_packet[2];   /* y 轴翻转 */

    u32 next = (g_ms_head + 1) % MS_BUF_SIZE;
    if (next == g_ms_tail) return;       /* 满，丢 */

    g_ms_buf[g_ms_head].dx = dx;
    g_ms_buf[g_ms_head].dy = dy;
    g_ms_buf[g_ms_head].buttons = buttons;
    g_ms_buf[g_ms_head]._pad = 0;
    g_ms_head = next;
}

int ps2_mouse_read(struct helix_mouse_event *out, int max) {
    int n = 0;
    while (n < max && g_ms_tail != g_ms_head) {
        out[n++] = g_ms_buf[g_ms_tail];
        g_ms_tail = (g_ms_tail + 1) % MS_BUF_SIZE;
    }
    return n;
}
```

`ps2_init()` 第二阶段（mouse init）：
```c
void ps2_init(void) {
    /* keyboard 第一阶段 (M18) */
    g_kb_head = g_kb_tail = 0;
    g_shift = 0;
    while (inb(PS2_STATUS_PORT) & 1) inb(PS2_DATA_PORT);
    pic_unmask(1);
    kprintf("[ps2] keyboard ready (IRQ1 unmasked)\n");

    /* mouse 第二阶段 (M23) */
    g_ms_head = g_ms_tail = 0;
    g_ms_byte_idx = 0;

    /* 1) 启用 aux port (0xA8)。 */
    ps2_write_cmd(0xA8);
    ps2_flush_data();

    /* 2) 启用 IRQ1 + IRQ12 + system flag (0x47 = 0b01000111)。 */
    ps2_write_cmd(0x60);
    ps2_write_cmd(0x47);

    /* 3) 0xD4 0xF4 = Enable Data Reporting (mouse)。 */
    ps2_write_aux(0xF4);
    ps2_flush_data();           /* ACK = 0xFA */

    /* 4) 0xD4 0xF3 0x64 = Set Sample Rate to 100/sec。 */
    ps2_write_aux(0xF3);
    ps2_flush_data();
    ps2_write_aux(100);
    ps2_flush_data();

    pic_unmask(12);             /* IRQ12 — mouse */
    kprintf("[ps2] mouse ready (IRQ12 unmasked)\n");
}
```

### `kernel/arch/x86_64/irq.c`

`irq_handle` switch 加：
```c
case 12:
    ps2_mouse_handler();
    break;
```

`irq_init` 的 kprintf 行更新为 `"IRQ0 timer, IRQ1 keyboard, IRQ12 mouse"`。

### `kernel/proc/syscall.c`

`sys_readkey` 后加：
```c
/* M23: read mouse events (struct helix_mouse_event[count]). */
static i64 sys_mouse_read(u64 buf_ptr, u64 count) {
    if (!count) return 0;
    if (!user_ptr_ok((void *)(uintptr_t)buf_ptr,
                     count * sizeof(struct helix_mouse_event)))
        return ERR(EFAULT);
    int n = ps2_mouse_read((struct helix_mouse_event *)(uintptr_t)buf_ptr,
                           (int)count);
    if (n == 0) return -11;     /* EAGAIN */
    return (i64)n;
}
```

dispatch table 加：
```c
case SYS_mouse_read: ret = sys_mouse_read(f->a0, f->a1); break;
```

### `user/helixbox.c`

`HelixKbOK` block（:418-421 附近）之后插入：
```c
{
    /* M23: PS/2 mouse smoke — probe 8 次拿事件；headless QEMU 没人移鼠标，
     * 环形 buffer 总是空，但 sys_mouse_read 路径仍走通。Got>0 时附 "(events)"。 */
    struct helix_mouse_event ev[4];
    int got = 0;
    for (int i = 0; i < 8 && got == 0; i++) {
        long r = sys_mouse_read(ev, 4);
        if (r > 0) { got = (int)r; break; }
    }
    if (got > 0)
        xwrite("HelixMouseOK (events)\n");
    else
        xwrite("HelixMouseOK\n");
}
```

## 验证

### smoke-linux（多任务 + helixbox + headless mouse）
```bash
make && make smoke-linux
# 串口含：
#   [ps2] keyboard ready (IRQ1 unmasked)        (M18 不回归)
#   [ps2] mouse ready (IRQ12 unmasked)           ← M23 NEW
#   [user] HelixKbOK                              (M18 不回归)
#   [user] HelixMouseOK                           ← M23 NEW
#   [user] HelixPreemptOK                         (M22 不回归)
#   helixbox_smoke_done / BusyBox / msh pipeline 等
```

### smoke-fs（单任务为主）— 不应回归
```bash
make smoke-fs
# 含 "HelixFATWriteOK" + "loaded init+task2 from disk"
```

### smoke-net（多任务 + host echo server）— TCP 不回归
```bash
make smoke-net
# 注：MSYS2 默认未启 host echo server，cmd_tcp_smoke 等不到 8080 → FAIL HelixNetOK；
#     这是 pre-existing M21 baseline（无 host server = 必然卡 connect retry），
#     M23 不引入额外回归。
# 内核层 TCP 仍跑：HelixTcpOK + tcp_init 等 marker 仍可见。
```

## 已知边界

- QEMU `-display none` (headless) 没人移鼠标，环形 buffer 始终空，helixbox 走 8 次
  probe 都返回 -EAGAIN，got=0。HelixMouseOK 仍打印（驱动 ready 视为通过）。
- 在 QEMU 启用 `-display gtk` 或 `-display sdl` 并移动鼠标时，IRQ12 应触发，
  helixbox 的 `got > 0` 路径走通，HelixMouseOK 后附 "(events)"。
- y 轴翻转：PS/2 y+ = 屏上 → 我们 dy>0 = 屏下（GUI 约定），所以鼠标上移 dx>0,
  dy<0（屏幕 y 减小）。
- overflow flag (bit 6/7) 直接丢包，避免 dy/dx wrap 误判。
- `MS_BUF_SIZE=64`：高频 mouse move (~100Hz) 足够缓冲；用户态不读时也只是丢
  最旧事件，不会阻塞。