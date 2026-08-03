# Goal — M18: fb user-space interface + PS/2 keyboard

## 验收

- `make smoke` 串口含 `HelixFBInfoOK` + `HelixFBMmapOK` + `HelixKbOK`
- `make smoke-fb` QEMU 窗口可见蓝色矩形（mmap 写入的测试图案）
- PS/2 键盘 IRQ1 正常触发，`[ps2] keyboard ready` 出现在启动日志

## 范围

- PS/2 键盘驱动：IRQ1 handler + scancode ring buffer + set-1 ASCII translation
- fb user-space：`fb_info` 系统调用 + `mmap(fd=-4)` 映射帧缓冲到用户空间
- `readkey` 系统调用：非阻塞 PS/2 键盘读取
- helixbox 自检：fb_info / mmap fb / readkey 三项
