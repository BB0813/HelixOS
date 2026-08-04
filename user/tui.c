/* HelixOS TUI shell — user-space mini-terminal using fb mmap + PS/2 keyboard
 *
 * M19: text grid (80x30 at 8x16 font), keyboard input, built-in commands.
 * No libc — freestanding. Talks to the kernel via syscall().
 */
#include "usys.h"

#define SYS_readkey   547
#define SYS_fb_info   546
#define SYS_mmap      9

#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define FB_FD_SENTINEL 0xFFFFFFFFFFFFFFFCLL  /* fd=-4 means fb mmap */

/* Color encoding (BGRA, native QEMU/OVMF pixel format) */
#define COLOR_BLACK   0x00000000u
#define COLOR_BLUE    0x00FF0000u
#define COLOR_GREEN   0x0000FF00u
#define COLOR_CYAN    0x00FFFF00u
#define COLOR_RED     0x000000FFu
#define COLOR_MAGENTA 0x00FF00FFu
#define COLOR_YELLOW  0x0000FFFFu
#define COLOR_WHITE   0x00FFFFFFu
#define COLOR_LIGHT_GRAY 0x00AAAAAAu
#define COLOR_DARK_GRAY  0x00555555u

#define FB_W_MAX  1024
#define FB_H_MAX  768
#define FONT_W    8
#define FONT_H    16
#define COLS_MAX  (FB_W_MAX / FONT_W)   /* 128 */
#define ROWS_MAX  (FB_H_MAX / FONT_H)   /* 48 */

struct fb_info_user {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned long long size;
};

/* 8x16 VGA bitmap font (ASCII 32-126 only) */
static const unsigned char font8x16[][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},       /* 32 ' ' */
    {0,0x18,0x3C,0x3C,0x3C,0x18,0x18,0,0x18,0x18,0,0,0,0,0,0},  /* 33 '!' */
    {0,0x6C,0x6C,0x6C,0,0,0,0,0,0,0,0,0,0,0,0},                  /* 34 '"' */
    {0,0,0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0,0,0,0,0,0,0},     /* 35 '#' */
    {0x18,0x18,0x7E,0xC0,0xC0,0x7C,0x06,0x06,0xFC,0x18,0x18,0,0,0,0,0}, /* 36 '$' */
    {0,0,0,0xC6,0xCC,0x18,0x30,0x60,0xC6,0x86,0,0,0,0,0,0},      /* 37 '%' */
    {0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0,0,0,0,0,0},/* 38 '&' */
    {0,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0,0,0,0},                  /* 39 ''' */
    {0,0x18,0x30,0x60,0x60,0x60,0x60,0x60,0x30,0x18,0,0,0,0,0,0},/* 40 '(' */
    {0,0x18,0x0C,0x06,0x06,0x06,0x06,0x06,0x0C,0x18,0,0,0,0,0,0},/* 41 ')' */
    {0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0,0,0},            /* 42 '*' */
    {0,0,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0,0,0,0,0,0,0},      /* 43 '+' */
    {0,0,0,0,0,0,0,0,0,0x18,0x18,0x18,0x30,0,0,0},              /* 44 ',' */
    {0,0,0,0,0,0,0xFE,0,0,0,0,0,0,0,0,0},                        /* 45 '-' */
    {0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0,0},                     /* 46 '.' */
    {0,0,0,0x06,0x0C,0x18,0x30,0x60,0xC0,0,0,0,0,0,0,0},         /* 47 '/' */
    {0,0x7C,0xC6,0xC6,0xCE,0xD6,0xE6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 48 '0' */
    {0,0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0,0},/* 49 '1' */
    {0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xFE,0,0,0,0,0,0},/* 50 '2' */
    {0,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},/* 51 '3' */
    {0,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x1E,0,0,0,0,0,0},/* 52 '4' */
    {0,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},/* 53 '5' */
    {0,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 54 '6' */
    {0,0xFE,0xC6,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0,0,0},/* 55 '7' */
    {0,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 56 '8' */
    {0,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0,0,0,0,0,0},/* 57 '9' */
    {0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0,0},              /* 58 ':' */
    {0,0,0,0x18,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0,0},            /* 59 ';' */
    {0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0,0},/* 60 '<' */
    {0,0,0,0,0,0xFE,0,0,0xFE,0,0,0,0,0,0,0},                     /* 61 '=' */
    {0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0,0,0},/* 62 '>' */
    {0,0x7C,0xC6,0x06,0x0C,0x18,0x18,0,0x18,0x18,0,0,0,0,0,0},  /* 63 '?' */
    {0,0x7C,0xC6,0xC6,0xDE,0xDE,0xDC,0xC0,0xC0,0x7C,0,0,0,0,0,0},/* 64 '@' */
    {0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0,0,0,0,0,0},/* 65 'A' */
    {0,0xFC,0xC6,0xC6,0xC6,0xFC,0xC6,0xC6,0xC6,0xFC,0,0,0,0,0,0},/* 66 'B' */
    {0,0x7C,0xC6,0xC0,0xC0,0xC0,0xC0,0xC0,0xC6,0x7C,0,0,0,0,0,0},/* 67 'C' */
    {0,0xFC,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xFC,0,0,0,0,0,0},/* 68 'D' */
    {0,0xFE,0xC0,0xC0,0xC0,0xF8,0xC0,0xC0,0xC0,0xFE,0,0,0,0,0,0},/* 69 'E' */
    {0,0xFE,0xC0,0xC0,0xC0,0xF8,0xC0,0xC0,0xC0,0xC0,0,0,0,0,0,0},/* 70 'F' */
    {0,0x7C,0xC6,0xC0,0xC0,0xCE,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 71 'G' */
    {0,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0,0},/* 72 'H' */
    {0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},/* 73 'I' */
    {0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0,0,0,0,0,0},/* 74 'J' */
    {0,0xE6,0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0xE6,0,0,0,0,0,0},/* 75 'K' */
    {0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC6,0xFE,0,0,0,0,0,0},/* 76 'L' */
    {0,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0,0},/* 77 'M' */
    {0,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0,0,0,0,0,0},/* 78 'N' */
    {0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 79 'O' */
    {0,0xFC,0xC6,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0xC0,0,0,0,0,0,0},/* 80 'P' */
    {0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06,0,0,0,0,0},/* 81 'Q' */
    {0,0xFC,0xC6,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0xC6,0,0,0,0,0,0},/* 82 'R' */
    {0,0x7C,0xC6,0xC0,0xC0,0x7C,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},/* 83 'S' */
    {0,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},/* 84 'T' */
    {0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},/* 85 'U' */
    {0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0,0,0},/* 86 'V' */
    {0,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x6C,0,0,0,0,0,0},/* 87 'W' */
    {0,0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0xC6,0,0,0,0,0,0},/* 88 'X' */
    {0,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},/* 89 'Y' */
    {0,0xFE,0xC6,0x8C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0,0,0,0,0,0},/* 90 'Z' */
    {0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0,0},/* 91 '[' */
    {0,0,0,0xC0,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0,0,0},         /* 92 '\' */
    {0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0,0},/* 93 ']' */
    {0,0x10,0x38,0x6C,0xC6,0,0,0,0,0,0,0,0,0,0,0},               /* 94 '^' */
    {0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0,0},                        /* 95 '_' */
    {0,0x30,0x30,0x18,0,0,0,0,0,0,0,0,0,0,0,0},                  /* 96 '`' */
    {0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0x76,0,0,0,0,0,0},         /* 97 'a' */
    {0,0xE0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0,0,0,0,0,0},/* 98 'b' */
    {0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC6,0x7C,0,0,0,0,0,0},         /* 99 'c' */
    {0,0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0,0},/*100 'd' */
    {0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0,0,0,0,0,0},         /*101 'e' */
    {0,0x38,0x6C,0x60,0x60,0xF8,0x60,0x60,0x60,0xF0,0,0,0,0,0,0},/*102 'f' */
    {0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0,0,0,0},   /*103 'g' */
    {0,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0,0,0,0,0,0},/*104 'h' */
    {0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},   /*105 'i' */
    {0,0x06,0x06,0,0x0E,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0,0,0,0},/*106 'j' */
    {0,0xE0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0xE6,0,0,0,0,0,0},/*107 'k' */
    {0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},/*108 'l' */
    {0,0,0,0,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0,0,0,0,0,0},         /*109 'm' */
    {0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0,0},         /*110 'n' */
    {0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},         /*111 'o' */
    {0,0,0,0,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0,0,0,0},   /*112 'p' */
    {0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0,0,0,0},   /*113 'q' */
    {0,0,0,0,0xDC,0x76,0x66,0x60,0x60,0xF0,0,0,0,0,0,0},         /*114 'r' */
    {0,0,0,0,0x7C,0xC6,0x70,0x0C,0xC6,0x7C,0,0,0,0,0,0},         /*115 's' */
    {0,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x36,0x1C,0,0,0,0,0,0},/*116 't' */
    {0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0,0},         /*117 'u' */
    {0,0,0,0,0xC3,0xC3,0x66,0x3C,0x18,0x18,0,0,0,0,0,0},         /*118 'v' */
    {0,0,0,0,0xC6,0xC6,0xD6,0xD6,0xD6,0x6C,0,0,0,0,0,0},         /*119 'w' */
    {0,0,0,0,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0,0,0,0,0,0},         /*120 'x' */
    {0,0,0,0,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0,0,0,0},   /*121 'y' */
    {0,0,0,0,0xFE,0xCC,0x18,0x30,0x66,0xFE,0,0,0,0,0,0},         /*122 'z' */
    {0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0,0,0,0,0,0},/*123 '{' */
    {0,0x18,0x18,0x18,0x18,0,0x18,0x18,0x18,0x18,0,0,0,0,0,0},   /*124 '|' */
    {0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0,0,0,0,0,0},/*125 '}' */
    {0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0,0},                     /*126 '~' */
};

/* Globals */
static unsigned int *fb_pixels;     /* pointer to mapped framebuffer (32bpp) */
static unsigned int fb_w, fb_h, fb_pitch;
static int cols, rows;              /* text grid size */
static char screen[ROWS_MAX][COLS_MAX + 1];   /* text grid */
static unsigned int screen_fg[ROWS_MAX][COLS_MAX]; /* fg BGRA color */
static unsigned int screen_bg[ROWS_MAX][COLS_MAX]; /* bg BGRA color */
static int cursor_x, cursor_y;     /* cursor position in chars */

/* syscall wrapper for 6 args */
static long syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rcx", "r11", "memory");
    return ret;
}

/* low-level write — write text to serial (and console stdout) for log backup */
static void log_write(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sys_write(1, s, n);
}

/* draw a single character to framebuffer at pixel (x, y) */
static void fb_draw_char_px(int px, int py, char ch, unsigned int fg, unsigned int bg)
{
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *glyph = font8x16[ch - 32];
    unsigned int *row_start = fb_pixels + py * (fb_pitch / 4) + px;
    for (int j = 0; j < FONT_H; j++) {
        unsigned int *p = row_start + j * (fb_pitch / 4);
        unsigned char bits = glyph[j];
        for (int i = 0; i < FONT_W; i++) {
            /* bit MSB-first (i=0 is leftmost) */
            p[i] = (bits & (0x80 >> i)) ? fg : bg;
        }
    }
}

/* draw a single character to framebuffer at grid (col, row) */
static void fb_draw_char(int col, int row, char ch, unsigned int fg, unsigned int bg)
{
    if (col < 0 || col >= cols || row < 0 || row >= rows) return;
    fb_draw_char_px(col * FONT_W, row * FONT_H, ch, fg, bg);
}

/* draw a string to framebuffer at grid (col, row) */
static void fb_draw_string(int col, int row, const char *s, unsigned int fg, unsigned int bg)
{
    int c = col;
    while (*s && c < cols) {
        fb_draw_char(c, row, *s, fg, bg);
        s++;
        c++;
    }
}

/* clear framebuffer to color */
static void fb_clear(unsigned int color)
{
    unsigned int total = fb_pitch / 4 * fb_h;
    for (unsigned int i = 0; i < total; i++)
        fb_pixels[i] = color;
}

/* scroll screen up by one row */
static void scroll_up(void)
{
    for (int r = 0; r < rows - 1; r++) {
        for (int c = 0; c < cols; c++) {
            screen[r][c] = screen[r + 1][c];
            screen_fg[r][c] = screen_fg[r + 1][c];
            screen_bg[r][c] = screen_bg[r + 1][c];
        }
    }
    /* clear last row */
    for (int c = 0; c < cols; c++) {
        screen[rows - 1][c] = ' ';
        screen_fg[rows - 1][c] = COLOR_LIGHT_GRAY >> 0;
        screen_bg[rows - 1][c] = COLOR_BLACK >> 0;
    }
}

/* render full screen from text grid to fb (currently unused — every char is drawn immediately) */
__attribute__((unused))
static void render(void)
{
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            fb_draw_char(c, r, screen[r][c], screen_fg[r][c], screen_bg[r][c]);
        }
    }
}

/* put a char to text grid (with newline handling) at cursor */
static void tui_putc(char ch)
{
    if (ch == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (ch == '\r') {
        cursor_x = 0;
    } else if (ch == '\b') {
        if (cursor_x > 0) cursor_x--;
        screen[cursor_y][cursor_x] = ' ';
        screen_fg[cursor_y][cursor_x] = COLOR_LIGHT_GRAY;
        screen_bg[cursor_y][cursor_x] = COLOR_BLACK;
        fb_draw_char(cursor_x, cursor_y, ' ', COLOR_LIGHT_GRAY, COLOR_BLACK);
        return;
    } else {
        screen[cursor_y][cursor_x] = ch;
        screen_fg[cursor_y][cursor_x] = COLOR_LIGHT_GRAY;
        screen_bg[cursor_y][cursor_x] = COLOR_BLACK;
        fb_draw_char(cursor_x, cursor_y, ch, COLOR_LIGHT_GRAY, COLOR_BLACK);
        cursor_x++;
    }
    if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= rows) {
        scroll_up();
        cursor_y = rows - 1;
    }
}

static void tui_puts(const char *s)
{
    while (*s) tui_putc(*s++);
}

/* init framebuffer */
static int tui_init(void)
{
    struct fb_info_user info;
    /* zero out struct (no memset in libc) */
    unsigned char *p = (unsigned char *)&info;
    for (unsigned int i = 0; i < sizeof(info); i++) p[i] = 0;
    long r = usyscall(SYS_fb_info, (long)&info, 0, 0);
    if (r < 0 || info.width == 0 || info.height == 0 || info.pitch == 0) {
        log_write("[tui] no framebuffer (GOP not available)\n");
        return -1;
    }
    fb_w = info.width;
    fb_h = info.height;
    fb_pitch = info.pitch;
    cols = fb_w / FONT_W;
    rows = fb_h / FONT_H;
    if (cols > COLS_MAX) cols = COLS_MAX;
    if (rows > ROWS_MAX) rows = ROWS_MAX;

    fb_pixels = (unsigned int *)syscall6(SYS_mmap, 0, (long)info.size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_ANONYMOUS | MAP_PRIVATE,
                                          FB_FD_SENTINEL, 0);
    if ((long)fb_pixels <= 0) {
        log_write("[tui] mmap fb failed\n");
        return -1;
    }
    return 0;
}

static void tui_clear_screen(void)
{
    fb_clear(COLOR_BLACK);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            screen[r][c] = ' ';
            screen_fg[r][c] = COLOR_LIGHT_GRAY;
            screen_bg[r][c] = COLOR_BLACK;
        }
    cursor_x = 0;
    cursor_y = 0;
}

/* draw top status bar */
static void draw_status_bar(void)
{
    fb_draw_string(0, 0, "HelixOS TUI shell (M19) — fb 8x16 text — Ctrl+D reboot, Ctrl+C exit",
                   COLOR_BLACK, COLOR_CYAN);
}

/* run an external command (execve helixbox if available) */
static void run_external(const char *cmd, const char *arg)
{
    /* For now: just print "running X" */
    tui_puts("[run] ");
    tui_puts(cmd);
    if (arg) {
        tui_puts(" ");
        tui_puts(arg);
    }
    tui_puts(" (not implemented in M19)\n");
}

static void cmd_help(void)
{
    tui_puts("Built-in commands:\n");
    tui_puts("  help         Show this message\n");
    tui_puts("  clear        Clear screen\n");
    tui_puts("  echo TEXT    Print TEXT\n");
    tui_puts("  ls [PATH]    List directory (delegates to helixbox)\n");
    tui_puts("  cat FILE     Print FILE contents (delegates)\n");
    tui_puts("  ps           List tasks (delegates)\n");
    tui_puts("  tcpstat      TCP socket stats (delegates)\n");
    tui_puts("  time         Show ticks since boot\n");
    tui_puts("  reboot       Triple-fault to reboot QEMU\n");
    tui_puts("  exit         Exit TUI (back to helix shell)\n");
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

/* dispatch a single command line */
static void dispatch(char *line)
{
    /* skip leading spaces */
    while (*line == ' ') line++;
    if (*line == 0) return;

    if (streq(line, "help")) { cmd_help(); return; }
    if (streq(line, "clear")) { tui_clear_screen(); draw_status_bar(); return; }
    if (streq(line, "exit")) { tui_puts("[tui] use Ctrl+D to reboot or Ctrl+C to exit\n"); return; }
    if (streq(line, "reboot")) {
        tui_puts("[tui] rebooting via triple-fault...\n");
        /* zero CR3 to trigger a triple fault — QEMU restarts */
        __asm__ volatile("xor %%rax, %%rax\n\t"
                         "mov %%rax, %%cr3\n\t"
                         ::: "memory");
        return;
    }
    if (starts_with(line, "echo ")) { tui_puts(line + 5); tui_putc('\n'); return; }
    if (streq(line, "echo")) { tui_putc('\n'); return; }

    if (streq(line, "ls")) { run_external("helixbox", "ls"); return; }
    if (starts_with(line, "ls ")) { run_external("helixbox", line); return; }
    if (streq(line, "cat")) { run_external("helixbox", "cat"); return; }
    if (starts_with(line, "cat ")) { run_external("helixbox", line); return; }
    if (streq(line, "ps")) { run_external("helixbox", "ps"); return; }
    if (streq(line, "tcpstat")) { run_external("helixbox", "tcpstat"); return; }
    if (streq(line, "time")) {
        tui_puts("uptime: (read /proc/uptime not implemented)\n");
        return;
    }

    tui_puts("tui: unknown command: ");
    tui_puts(line);
    tui_putc('\n');
    tui_puts("type 'help' for list\n");
}

/* read a line of input with editing; returns length, or -1 on Ctrl+D */
static int read_line(char *buf, int maxlen)
{
    int n = 0;
    cursor_x = 0;
    cursor_y = rows - 1;
    fb_draw_string(0, rows - 1, "> ", COLOR_GREEN, COLOR_BLACK);
    cursor_x = 2;
    for (;;) {
        char kbuf[4] = {0};
        long r = usyscall(SYS_readkey, (long)kbuf, 4, 0);
        if (r == -11) {
            /* EAGAIN — yield and retry */
            yield();
            continue;
        }
        if (r <= 0) continue;
        char ch = kbuf[0];
        if (ch == 0) continue;
        if (ch == 4) return -1; /* Ctrl+D */
        if (ch == 3) return -2; /* Ctrl+C */
        if (ch == '\n' || ch == '\r') {
            tui_putc('\n');
            buf[n] = 0;
            return n;
        }
        if (ch == '\b' || ch == 127) {
            if (n > 0) {
                n--;
                /* backspace: move cursor left, overwrite with space */
                if (cursor_x > 2) cursor_x--;
                fb_draw_char(cursor_x, rows - 1, ' ', COLOR_GREEN, COLOR_BLACK);
            }
            continue;
        }
        if (n + 1 < maxlen && ch >= 32 && ch < 127) {
            buf[n++] = ch;
            tui_putc(ch);
        }
    }
}

/* Linux process entry: rsp -> argc, argv pointers, ...
 * No CRT — read argc/argv from the stack the kernel built. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "xor %%rdi, %%rdi\n\t"
        "xor %%rsi, %%rsi\n\t"
        "call tui_main\n\t"
        "mov %%rax, %%rdi\n\t"
        "mov $231, %%eax\n\t"          /* exit_group */
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        :
        : "memory");
}

void tui_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    log_write("[tui] starting\n");
    if (tui_init() < 0) {
        log_write("[tui] init failed — exiting\n");
        sys_exit(1);
    }

    tui_clear_screen();
    draw_status_bar();
    tui_puts("\n");
    tui_puts("  HelixOS TUI shell\n");
    tui_puts("  Built on framebuffer + PS/2 keyboard (M19)\n");
    tui_puts("  Type 'help' for commands.\n\n");

    char line[128];
    for (;;) {
        int n = read_line(line, sizeof(line));
        if (n == -1 || n == -2) {
            tui_puts("[tui] exiting\n");
            sys_exit(0);
        }
        dispatch(line);
    }
}
