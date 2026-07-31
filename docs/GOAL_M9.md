# Goal: HelixOS M9 — GOP 帧缓冲（历史）

> **状态：已完成（2026-08-01）** — `make smoke-fb`；`fb_smoke_done` 或 headless fallback。

## 范围

1. UEFI `EFI_GRAPHICS_OUTPUT_PROTOCOL`：ExitBootServices 前 LocateProtocol，选 ≥640 宽模式，写入 `boot_info`。
2. `kernel/drv/fb.c`：`fb_init` / `fb_cls` / `fb_pixel` / `fb_rect` / `fb_put_char` / `fb_puts` + 8×16 VGA 字体。
3. `kernel_early_main` 绘制测试图案；串口 `[fb] fb_smoke_done`。
4. 独立 `make smoke-fb`；不挡 CLI/net smoke。

## 约束 / 非目标

- 多数发行版 OVMF 4MB **无** QemuVideoDxe → `EFI_NOT_FOUND` 时 graceful headless。
- 不做 Wayland / GPU 加速 / 桌面。

## 验收

`make smoke-fb`：`M9 framebuffer ready` + `fb_smoke_done`，或 `M9 no framebuffer` headless OK。
