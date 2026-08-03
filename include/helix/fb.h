/* M9 minimal framebuffer (GOP) — pixel ops + embedded 8x16 bitmap font. */
#pragma once

#include "helix/types.h"
#include "helix/boot_info.h"

int  fb_init(struct helix_boot_info *info);
void fb_cls(u32 color);
void fb_pixel(int x, int y, u32 color);
void fb_rect(int x, int y, int w, int h, u32 color);
void fb_put_char(int x, int y, char c, u32 fg, u32 bg);
void fb_puts(int x, int y, const char *s, u32 fg, u32 bg);

/* M18: user-space framebuffer interface */
void fb_get_info(u32 *w, u32 *h, u32 *pitch, u32 *bpp, u64 *size);
int  fb_map_user(u64 va);  /* map fb physical pages to user VA; 0=ok, -1=fail */
