#include "kernel.h"
#include "kmon.h"
#include "console.h"
#include "input.h"
#include "string.h"
#include "pmm.h"
#include "proc.h"
#include "timer.h"
#include "vfs.h"
#include "uproc.h"
#include "power.h"
#include "rtc.h"

/* Rescue console. The kernel falls back to this when userspace cannot be
 * started (no filesystem, missing or corrupt /bin/init) so the machine
 * stays inspectable instead of sitting at a dead prompt. */

#define LINE_MAX 128
#define ARGS_MAX 4

static void readline(char *buf, int max)
{
    int len = 0;

    for (;;) {
        int c = input_getc();
        if (c == '\n') {
            kprintf("\n");
            buf[len] = '\0';
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                kprintf("\b \b");
            }
            continue;
        }
        if (c >= 0x80 || c < ' ')
            continue;
        if (len < max - 1) {
            buf[len++] = (char)c;
            kprintf("%c", c);
        }
    }
}

static int split(char *line, char *argv[], int max)
{
    int argc = 0;

    while (*line && argc < max) {
        while (*line == ' ')
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ')
            line++;
    }
    return argc;
}

static const char *state_name(enum task_state s)
{
    switch (s) {
    case TASK_RUNNING:  return "running";
    case TASK_RUNNABLE: return "ready";
    case TASK_SLEEPING: return "sleeping";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "?";
    }
}

static void cmd_mem(void)
{
    uint64_t total = pmm_total_pages(), free = pmm_free_pages();
    kprintf("pages: %lu total, %lu free, %lu used\n", total, free, total - free);
    kprintf("bytes: %lu MiB total, %lu MiB free\n",
            (uint64_t)(total * PAGE_SIZE / (1024 * 1024)),
            (uint64_t)(free * PAGE_SIZE / (1024 * 1024)));
}

static void cmd_ps(void)
{
    kprintf("  PID  STATE     NAME\n");
    for (struct task *t = task_all_list(); t; t = t->allnext)
        kprintf("  %3d  %-8s  %s\n", t->pid, state_name(t->state), t->name);
}

static void cmd_ls(const char *path)
{
    struct k_dirent de;

    for (int i = 0; ; i++) {
        if (vfs_readdir(path, i, &de) < 0)
            break;
        kprintf("  %c %8u  %s\n", de.is_dir ? 'd' : '-', de.size, de.name);
    }
}

static void cmd_cat(const char *path)
{
    struct file *f = vfs_open(path, O_RDONLY);
    if (!f) {
        kprintf("kmon: cannot open %s\n", path);
        return;
    }
    char buf[256];
    long n;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0)
        for (long i = 0; i < n; i++)
            kprintf("%c", buf[i]);
    vfs_close(f);
}

static void cmd_help(void)
{
    kprintf("kmon commands:\n"
            "  help            this list\n"
            "  mem             physical memory statistics\n"
            "  ps              kernel task list\n"
            "  ls [path]       list a directory (needs a mounted fs)\n"
            "  cat <path>      print a file\n"
            "  run <path>      start a user program\n"
            "  date            read the hardware clock\n"
            "  uptime          time since boot\n"
            "  reboot | halt   power control\n");
}

void kmon_run(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    char *argv[ARGS_MAX];

    console_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("\nkmon: kernel rescue console (userspace unavailable)\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("type 'help' for commands\n");

    for (;;) {
        kprintf("kmon> ");
        readline(line, sizeof(line));
        int argc = split(line, argv, ARGS_MAX);
        if (argc == 0)
            continue;

        if (!strcmp(argv[0], "help")) {
            cmd_help();
        } else if (!strcmp(argv[0], "mem")) {
            cmd_mem();
        } else if (!strcmp(argv[0], "ps")) {
            cmd_ps();
        } else if (!strcmp(argv[0], "ls")) {
            cmd_ls(argc > 1 ? argv[1] : "/");
        } else if (!strcmp(argv[0], "cat")) {
            if (argc > 1)
                cmd_cat(argv[1]);
            else
                kprintf("usage: cat <path>\n");
        } else if (!strcmp(argv[0], "run")) {
            if (argc > 1) {
                char *uargv[2] = { argv[1], NULL };
                int pid = uproc_spawn(argv[1], uargv, 1);
                if (pid < 0)
                    kprintf("kmon: cannot start %s\n", argv[1]);
                else
                    kprintf("kmon: started pid %d\n", pid);
            } else {
                kprintf("usage: run <path>\n");
            }
        } else if (!strcmp(argv[0], "date")) {
            char when[40];
            if (rtc_format(when, sizeof(when)) == 0)
                kprintf("%s\n", when);
            else
                kprintf("kmon: no usable clock\n");
        } else if (!strcmp(argv[0], "uptime")) {
            uint64_t ms = timer_ticks() * (1000 / TIMER_HZ);
            kprintf("up %lu.%02lu s\n", ms / 1000, (ms % 1000) / 10);
        } else if (!strcmp(argv[0], "reboot")) {
            kprintf("rebooting...\n");
            power_reboot();
        } else if (!strcmp(argv[0], "halt")) {
            power_halt();
        } else {
            kprintf("kmon: unknown command '%s' (try 'help')\n", argv[0]);
        }
    }
}
