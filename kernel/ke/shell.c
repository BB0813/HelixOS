#include "helix/shell.h"
#include "helix/kprintf.h"
#include "helix/serial.h"
#include "helix/pmm.h"
#include "helix/paging.h"
#include "helix/timer.h"
#include "helix/irq.h"
#include "helix/vfs.h"
#include "helix/string.h"
#include "helix/cpuio.h"

#define LINE_MAX 128

static struct helix_boot_info *g_info;
static char g_line[LINE_MAX];
static unsigned g_len;
static int g_prompted;

void shell_print_prompt(void)
{
    kprintf("helix> ");
    g_prompted = 1;
}

static void cmd_help(void)
{
    kprintf("Helix kernel shell (M2/M4)\n");
    kprintf("  help     — this list\n");
    kprintf("  mem      — PMM / heap summary\n");
    kprintf("  page     — identity map / ceiling\n");
    kprintf("  int      — IRQ + timer stats\n");
    kprintf("  uptime   — ticks since timer init\n");
    kprintf("  ls       — list root directory (VFS)\n");
    kprintf("  cat PATH — read file via VFS\n");
    kprintf("  tui      — launch M19 TUI shell (fb + PS/2)\n");
    kprintf("  halt     — cli; hlt loop\n");
}

static void ls_cb(const char *name, u64 size, void *user)
{
    (void)user;
    kprintf("  %s  %llu\n", name, (unsigned long long)size);
}

static void cmd_ls(void)
{
    kprintf("root:\n");
    if (vfs_root_list(ls_cb, 0) != 0)
        kprintf("  (vfs list failed)\n");
}

static void cmd_cat(const char *path)
{
    if (!path || !*path) {
        kprintf("usage: cat PATH\n");
        return;
    }
    char buf[256];
    u64 n = 0;
    if (vfs_read_all(path, buf, sizeof(buf) - 1, &n) != 0) {
        kprintf("cat: cannot open %s\n", path);
        return;
    }
    buf[n] = 0;
    kprintf("%s", buf);
    if (n && buf[n - 1] != '\n')
        kprintf("\n");
}

static void cmd_mem(void)
{
    u64 total = pmm_total_pages();
    u64 free  = pmm_free_pages_count();
    u64 ceil  = pmm_phys_ceiling();
    kprintf("PMM: total_pages=%llu free_pages=%llu used=%llu\n",
            (unsigned long long)total,
            (unsigned long long)free,
            (unsigned long long)(total - free));
    kprintf("PMM: phys_ceiling=0x%llx (%llu MiB span)\n",
            (unsigned long long)ceil,
            (unsigned long long)(ceil / (1024 * 1024)));
    if (g_info) {
        u64 conv = 0;
        for (u64 i = 0; i < g_info->mmap_count; i++) {
            if (g_info->mmap[i].type == 7)
                conv += g_info->mmap[i].npages * 4096ull;
        }
        kprintf("Boot: mmap_entries=%llu conventional≈%llu MiB\n",
                (unsigned long long)g_info->mmap_count,
                (unsigned long long)(conv / (1024 * 1024)));
    }
    kprintf("Heap: early pool 256 KiB (see boot log [heap])\n");
}

static void cmd_page(void)
{
    kprintf("Paging: identity map 0..0x%llx (2MiB pages)\n",
            (unsigned long long)paging_mapped_ceiling());
    kprintf("PMM ceiling: 0x%llx\n",
            (unsigned long long)pmm_phys_ceiling());
    if (g_info)
        kprintf("Boot phys_ceiling: 0x%llx\n",
                (unsigned long long)g_info->phys_ceiling);
}

static void cmd_int(void)
{
    kprintf("timer: ticks=%llu hz=%u\n",
            (unsigned long long)timer_ticks(),
            (unsigned)timer_hz());
    irq_dump_stats();
}

static void cmd_uptime(void)
{
    u64 t = timer_ticks();
    u32 hz = timer_hz();
    u64 sec = hz ? t / hz : 0;
    kprintf("uptime: %llu ticks (~%llu s at %u Hz)\n",
            (unsigned long long)t,
            (unsigned long long)sec,
            (unsigned)hz);
}

static void cmd_halt(void)
{
    kprintf("halting.\n");
    cpu_cli();
    for (;;)
        cpu_halt();
}

static void run_line(char *line)
{
    /* trim leading spaces */
    while (*line == ' ' || *line == '\t')
        line++;
    /* trim trailing */
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;
    if (*line == 0)
        return;

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0)
        cmd_help();
    else if (strcmp(line, "mem") == 0)
        cmd_mem();
    else if (strcmp(line, "page") == 0)
        cmd_page();
    else if (strcmp(line, "int") == 0)
        cmd_int();
    else if (strcmp(line, "uptime") == 0)
        cmd_uptime();
    else if (strcmp(line, "ls") == 0)
        cmd_ls();
    else if (strncmp(line, "cat ", 4) == 0)
        cmd_cat(line + 4);
    else if (strcmp(line, "cat") == 0)
        cmd_cat(0);
    else if (strcmp(line, "halt") == 0)
        cmd_halt();
    else if (strcmp(line, "tui") == 0) {
        /* M19: launch TUI shell from disk */
        extern struct task *task_exec_path(const char *name, const char *path, const char *const argv[]);
        const char *av[] = { "tui", 0 };
        struct task *t = task_exec_path("tui", "/bin/tui", av);
        kprintf("[shell] task_exec_path(tui) -> %p\n", (void *)t);
    }
    else
        kprintf("unknown command: %s (try help)\n", line);
}

void shell_init(struct helix_boot_info *info)
{
    g_info = info;
    g_len = 0;
    g_prompted = 0;
    kprintf("[shell] COM1 line editor ready\n");
    shell_print_prompt();
}

void shell_poll(void)
{
    char c;
    while (serial_poll_char(&c)) {
        if (!g_prompted)
            shell_print_prompt();

        /* normalize CR/LF */
        if (c == '\r')
            c = '\n';

        if (c == '\n') {
            serial_putchar('\n');
            g_line[g_len] = 0;
            run_line(g_line);
            g_len = 0;
            shell_print_prompt();
            continue;
        }

        if (c == 0x7F || c == 0x08) { /* backspace */
            if (g_len > 0) {
                g_len--;
                serial_puts("\b \b");
            }
            continue;
        }

        if (c < 32)
            continue; /* ignore other controls */

        if (g_len + 1 < LINE_MAX) {
            g_line[g_len++] = c;
            serial_putchar(c);
        }
    }
}
