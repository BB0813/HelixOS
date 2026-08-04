# Goal: M19 — TUI shell (fb + PS/2 keyboard)

## Context

M18 提供 `sys_fb_info`(546) + `sys_readkey`(547) + `sys_mmap(fd=-4)` 把 GOP 帧缓冲
暴露给用户态；helixbox 自检 (`HelixFBInfoOK` / `HelixFBMmapOK` / `HelixKbOK`)
证明接口已通。本里程碑把这条通路变成真正可用的 **用户态 TUI shell**：

- 在 framebuffer 上画文本 UI（status bar + 输出区 + 输入栏）
- 从 PS/2 键盘读键，交给行编辑器
- 内置命令（help/clear/echo/ls/cat/ps/tcpstat/time/reboot/exit）

## Files

- `user/tui.c` — 新建（freestanding，无 libc）
- `user/tui.ld` — 新建（0x40000000，与 helixbox 同地址）
- `Makefile` — `USER_TUI_ELF := $(BUILD)/user/tui.elf` + 编译/链接规则
- `scripts/mkesp.sh` — `bin/tui` 写入 ESP
- `kernel/ke/shell.c` — `tui` 命令经 `task_exec_path("tui","/bin/tui",av)` 启动
- `docs/ROADMAP.md` / `docs/ARCHITECTURE.md` / `docs/SYSCALLS.md`

## 设计要点

### 字体（嵌入 8x16 VGA bitmap）

ASCII 32–126 共 95 字符 × 16 行像素 = 95 × 16 字节。MSB-first：`bit & (0x80 >> i)`。

### 渲染

- 文本网格 `screen[ROWS_MAX][COLS_MAX+1]` 存 char
- `screen_fg/bg[ROWS_MAX][COLS_MAX]` 存 BGRA 颜色
- `fb_draw_char_px(px, py, ch, fg, bg)` 直接写 framebuffer 像素
- 行尾换行；满行 `scroll_up()` 把所有行上移

### 输入

- `sys_readkey(547)` 阻塞读取；EAGAIN → `yield()` 后重试
- 处理：Backspace (0x08/0x7F) / Enter (0x0A/0x0D) / Ctrl+D (0x04) / Ctrl+C (0x03)
- 其它 ASCII (32..126) 追加到缓冲区

### 内置命令

- `help` — 列出命令
- `clear` — 清屏，重画 status bar
- `echo TEXT` — 输出文本
- `ls` / `cat FILE` / `ps` / `tcpstat` — 占位（"not implemented in M19"），
  后续可经 `execve` 调用 helixbox
- `time` — 占位（读 `/proc/uptime` 未实现）
- `reboot` — `xor %rax,%rax; mov %rax,%cr3` 触发三重故障
- `exit` — `sys_exit(0)` 退出 TUI（实际提示用 Ctrl+D/Ctrl+C）

### 启动流程

1. `tui_init()`：sys_fb_info 拿宽高，sys_mmap(fd=-4) 映射 fb
2. `tui_clear_screen()` + `draw_status_bar()`
3. `read_line()` 进入读循环：sys_readkey → 行编辑 → 回车 → `dispatch(line)`

### 异常路径

- `sys_fb_info` 返回 width=0 → GOP 不可用（OVMF 4MB）→ `[tui] no framebuffer` 退出
- `sys_mmap` 返回 <=0 → `[tui] mmap fb failed` 退出
- EAGAIN on readkey → `yield()` → 重试

## 验证

```bash
make user             # build/user/tui.elf 应为 ~11 KiB
make esp              # 串口含 `+ bin/tui (11328 bytes from build/user/tui.elf)`
```

**功能验证**：MSYS2/ArchLinux/Ubuntu 打包的 4MB OVMF 不含 QemuVideoDxe，本地
QEMU 测试时 `fb_init` 失败，tui 优雅退出。要看到 tui UI 需用原生 OVMF（8MB+）
或真机固件。

内核 shell 中手动输入 `tui` 命令：

```
helix> tui
[shell] task_exec_path(tui) -> 0xffff...
```

任务已创建，fb 缺失环境下 `[tui] no framebuffer` 然后退出。