/*
 * M18 PS/2 keyboard driver.
 * - IRQ1 handler reads scancode from port 0x60
 * - Ring buffer stores translated ASCII characters
 * - Scancode set 1 → ASCII with shift support
 */
#include "helix/ps2.h"
#include "helix/cpuio.h"
#include "helix/pic.h"
#include "helix/kprintf.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define KB_BUF_SIZE     256

static u8  g_kb_buf[KB_BUF_SIZE];
static u32 g_kb_head, g_kb_tail;
static int g_shift;  /* 1 = shift held */

/* Scancode set 1 → ASCII: indices 0–127, 0 = unmapped. */
static const u8 g_sc1[128] = {
/*       0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F */
/*0*/    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',    8,    9,
/*1*/  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',   13,    0,    0,    0,
/*2*/  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',    0, '\\',   0,    0,
/*3*/  'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',    0, '*',   0,  ' ',   0,    0,
/*4*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*5*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*6*/    0,    0, '-',    0, '5',    0, '+',    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*7*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
};

/* Shifted variant: indices 0–127. */
static const u8 g_sc1_sh[128] = {
/*       0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F */
/*0*/    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',    8,    9,
/*1*/  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',   13,    0,    0,    0,
/*2*/  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',    0, '|',   0,    0,
/*3*/  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',    0, '*',   0,  ' ',   0,    0,
/*4*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*5*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*6*/    0,    0, '-',    0, '5',    0, '+',    0,    0,    0,    0,    0,    0,    0,    0,    0,
/*7*/    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
};

static inline void kb_put(u8 c)
{
    u32 next = (g_kb_head + 1) % KB_BUF_SIZE;
    if (next != g_kb_tail) {
        g_kb_buf[g_kb_head] = c;
        g_kb_head = next;
    }
}

void ps2_handler(void)
{
    u8 sc = inb(PS2_DATA_PORT);

    /* Shift press/release */
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return; }

    /* Ignore break codes (bit 7 set) and invalid */
    if (sc & 0x80 || sc >= 128) return;

    char c = g_shift ? g_sc1_sh[sc] : g_sc1[sc];
    if (c) kb_put((u8)c);
}

void ps2_init(void)
{
    g_kb_head = g_kb_tail = 0;
    g_shift = 0;
    /* Flush any pending scancodes */
    while (inb(PS2_STATUS_PORT) & 1)
        inb(PS2_DATA_PORT);
    pic_unmask(1);  /* IRQ1 — keyboard */
    kprintf("[ps2] keyboard ready (IRQ1 unmasked)\n");
}

int ps2_read(char *buf, int len)
{
    int n = 0;
    while (n < len && g_kb_tail != g_kb_head) {
        buf[n++] = (char)g_kb_buf[g_kb_tail];
        g_kb_tail = (g_kb_tail + 1) % KB_BUF_SIZE;
    }
    return n;
}
