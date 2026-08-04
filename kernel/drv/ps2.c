/*
 * M18 PS/2 keyboard driver.
 * - IRQ1 handler reads scancode from port 0x60
 * - Ring buffer stores translated ASCII characters
 * - Scancode set 1 → ASCII with shift support
 *
 * M23: extended to PS/2 mouse (aux port, IRQ12).
 * - ps2_write_cmd / ps2_write_aux / ps2_read_data helpers
 * - ps2_mouse_handler accumulates 3-byte packet → ring buffer
 * - ps2_init() phase 2 enables aux port + IRQ12
 */
#include "helix/ps2.h"
#include "helix/cpuio.h"
#include "helix/pic.h"
#include "helix/kprintf.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64
#define KB_BUF_SIZE     256
#define MS_BUF_SIZE     64

static u8  g_kb_buf[KB_BUF_SIZE];
static u32 g_kb_head, g_kb_tail;
static int g_shift;  /* 1 = shift held */

/* M23: mouse state */
static struct helix_mouse_event g_ms_buf[MS_BUF_SIZE];
static u32 g_ms_head, g_ms_tail;
static u8  g_ms_packet[3];
static u8  g_ms_byte_idx;

/* D3: extended scancode 0xE0 prefix pending. */
static u8  g_e0_pending;

/* Wait for status register to clear input buffer (bit 1 = input full). */
static void ps2_wait_input(void)
{
    int spins = 0;
    while ((inb(PS2_STATUS_PORT) & 0x02) && spins < 100000)
        spins++;
}

/* Send a command byte to PS/2 controller. */
static void ps2_write_cmd(u8 c)
{
    ps2_wait_input();
    outb(PS2_CMD_PORT, c);
}

/* Send a data byte to the aux (mouse) device via 0xD4 prefix. */
static void ps2_write_aux(u8 c)
{
    ps2_write_cmd(0xD4);
    ps2_wait_input();
    outb(PS2_DATA_PORT, c);
}

/* Flush any pending data byte (response ACK / error). */
static void ps2_flush_data(void)
{
    if (inb(PS2_STATUS_PORT) & 0x01)
        inb(PS2_DATA_PORT);
}

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

    /* D3: 0xE0 prefix — extended scancodes. Bit 7 set on 0xE0 was previously
     * dropped at the `sc & 0x80` filter below; capture it and translate the
     * following byte into an xterm-style ESC [ X sequence for arrow keys. */
    if (sc == 0xE0) { g_e0_pending = 1; return; }

    if (g_e0_pending) {
        g_e0_pending = 0;
        /* xterm: ESC [ A=up, B=down, C=right, D=left.
         * PS/2 make codes: up=0x48, down=0x50, left=0x4B, right=0x4D. */
        char final = 0;
        switch (sc) {
            case 0x48: final = 'A'; break;
            case 0x50: final = 'B'; break;
            case 0x4B: final = 'D'; break;
            case 0x4D: final = 'C'; break;
        }
        if (final) {
            kb_put(0x1B);
            kb_put('[');
            kb_put(final);
        }
        /* 0xE0 prefix break codes (0xC8/0xD0/0xCB/0xCD etc.) are swallowed. */
        return;
    }

    /* Ignore break codes (bit 7 set) and invalid */
    if (sc & 0x80 || sc >= 128) return;

    char c = g_shift ? g_sc1_sh[sc] : g_sc1[sc];
    if (c) kb_put((u8)c);
}

void ps2_init(void)
{
    g_kb_head = g_kb_tail = 0;
    g_shift = 0;
    g_e0_pending = 0;
    /* Flush any pending scancodes */
    while (inb(PS2_STATUS_PORT) & 1)
        inb(PS2_DATA_PORT);
    pic_unmask(1);  /* IRQ1 — keyboard */
    kprintf("[ps2] keyboard ready (IRQ1 unmasked)\n");

    /* M23: mouse phase 2 — enable aux port + IRQ12 + data reporting. */
    g_ms_head = g_ms_tail = 0;
    g_ms_byte_idx = 0;

    /* Enable auxiliary device (0xA8). */
    ps2_write_cmd(0xA8);
    ps2_flush_data();

    /* Set command byte: 0x60 then arg 0x47 = enable IRQ1 + IRQ12 + system flag. */
    ps2_write_cmd(0x60);
    ps2_write_cmd(0x47);

    /* 0xF4 = Enable Data Reporting (mouse). */
    ps2_write_aux(0xF4);
    ps2_flush_data(); /* ACK 0xFA */

    /* Set sample rate to 100/sec (0xF3 0x64). Optional but conventional. */
    ps2_write_aux(0xF3);
    ps2_flush_data();
    ps2_write_aux(100);
    ps2_flush_data();

    pic_unmask(12);  /* IRQ12 — mouse */
    kprintf("[ps2] mouse ready (IRQ12 unmasked)\n");
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

/* M23: PS/2 mouse IRQ12 handler.
 * Standard PS/2 mouse packet: 3 bytes [buttons|overflow, dx, dy].
 * bit 0=左键, bit 1=右键, bit 2=中键; bits 6/7=Y/X overflow (drop packet).
 * dx,dy are signed 8-bit; PS/2 y positive = "up the screen" — flip so
 * dy>0 = "down" matches GUI convention. */
void ps2_mouse_handler(void)
{
    u8 b = inb(PS2_DATA_PORT);

    /* Skip 0xAA / 0xFA / 0xFE etc. responses when state machine is idle. */
    if (g_ms_byte_idx == 0 && (b == 0xAA || b == 0xFA ||
                               b == 0xFE || b == 0xFF)) {
        return;
    }

    g_ms_packet[g_ms_byte_idx++] = b;
    if (g_ms_byte_idx < 3)
        return;
    g_ms_byte_idx = 0;

    u8 status = g_ms_packet[0];
    /* Drop packets with overflow flags set. */
    if (status & 0xC0) return;

    u8 buttons = status & 0x07;
    i16 dx = (i16)(i8)g_ms_packet[1];
    i16 dy = -(i16)(i8)g_ms_packet[2]; /* flip y axis */

    u32 next = (g_ms_head + 1) % MS_BUF_SIZE;
    if (next == g_ms_tail)
        return; /* ring buffer full, drop oldest unread first */

    g_ms_buf[g_ms_head].dx = dx;
    g_ms_buf[g_ms_head].dy = dy;
    g_ms_buf[g_ms_head].buttons = buttons;
    g_ms_buf[g_ms_head]._pad = 0;
    g_ms_head = next;
}

int ps2_mouse_read(struct helix_mouse_event *out, int max)
{
    int n = 0;
    while (n < max && g_ms_tail != g_ms_head) {
        out[n++] = g_ms_buf[g_ms_tail];
        g_ms_tail = (g_ms_tail + 1) % MS_BUF_SIZE;
    }
    return n;
}
