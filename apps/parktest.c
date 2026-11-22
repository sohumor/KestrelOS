/* parktest.c - Hangar self-check used by the reboot test.
 *
 * Builds state in every place a parked process keeps it -- heap, stack,
 * an SSE register, an open file offset, window pixels, a signal mask --
 * prints "PARKTEST READY pid=N", then idles in its event loop. When it
 * notices it is running under a new pid (it was parked and resumed) it
 * verifies all of it and prints "PARKTEST RESUMED OK" or what broke.
 *
 *   parktest            the normal check
 *   parktest --pipe     also hold a pipe, which Hangar must refuse
 *   parktest --tcp P    also hold a TCP connection to 10.0.2.2:P
 *   parktest --bg       minimize the window, so a focused window does not
 *                       take the console's keystrokes (serial tests) */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define HEAP_WORDS (64 * 1024)
#define STACK_WORDS 1024
#define DATA_PATH "/tmp/parktest.dat"
#define XMM_MAGIC 0x4B45535452454C21ULL   /* "KESTREL!" */

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t sum_words(const uint32_t *p, int n)
{
    uint32_t h = 0x811C9DC5U;
    for (int i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x01000193U;
    return h;
}

static void xmm_set(uint64_t v)
{
    __asm__ volatile("movq %0, %%xmm15" : : "r"(v) : "xmm15");
}

static uint64_t xmm_get(void)
{
    uint64_t v;
    __asm__ volatile("movq %%xmm15, %0" : "=r"(v));
    return v;
}

static void paint(gui_window *win, uint32_t seed)
{
    for (int y = 0; y < win->h; y++)
        for (int x = 0; x < win->w; x++)
            win->px[y * win->w + x] =
                0xFF000000U | ((uint32_t)(x * 3 + seed) & 0xFF) << 16 |
                ((uint32_t)(y * 5) & 0xFF) << 8 | (seed & 0x7F);
}

static int prepare_data_file(void)
{
    int fd;
    char line[16];

    mkdir_("/tmp");
    fd = open(DATA_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    for (int i = 0; i < 8; i++) {
        snprintf(line, sizeof(line), "line%d\n", i);
        write(fd, line, strlen(line));
    }
    close(fd);
    return open(DATA_PATH, O_RDONLY);
}

int main(int argc, char **argv)
{
    uint32_t stack_pattern[STACK_WORDS];
    uint32_t *heap;
    uint32_t heap_sum, stack_sum, win_sum, seed;
    uint64_t mask = 1ULL << (SIGUSR1 - 1), cur_mask = 0;
    gui_window *win;
    struct k_event ev;
    char buf[8];
    int fd, pid, extra_fds[2] = { -1, -1 }, background = 0;
    unsigned long counter = 0;

    seed = (uint32_t)uptime_ms() | 1U;
    rng_state = seed;
    pid = getpid();

    for (int i = 1; i < argc; i++) {
        if (!argv[i][0]) {
            continue;
        } else if (!strcmp(argv[i], "--bg")) {
            background = 1;
        } else if (!strcmp(argv[i], "--pipe")) {
            if (pipe_(extra_fds) < 0) {
                printf("parktest: pipe failed\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--tcp") && i + 1 < argc) {
            long h = syscall(SYS_TCP_CONNECT, (long)ip_aton("10.0.2.2"),
                             atoi(argv[++i]), 5000, 0);
            if (h < 0) {
                printf("parktest: tcp connect failed\n");
                return 1;
            }
        } else {
            printf("parktest: unknown option '%s'\n", argv[i]);
            printf("usage: parktest [--bg] [--pipe] [--tcp PORT]\n");
            return 2;
        }
    }

    heap = malloc(HEAP_WORDS * sizeof(uint32_t));
    if (!heap)
        return 1;
    for (int i = 0; i < HEAP_WORDS; i++)
        heap[i] = rng();
    heap_sum = sum_words(heap, HEAP_WORDS);
    for (int i = 0; i < STACK_WORDS; i++)
        stack_pattern[i] = rng();
    stack_sum = sum_words(stack_pattern, STACK_WORDS);

    fd = prepare_data_file();
    if (fd < 0 || read(fd, buf, 6) != 6 || memcmp(buf, "line0\n", 6) != 0) {
        printf("parktest: cannot prepare %s\n", DATA_PATH);
        return 1;
    }
    sigprocmask_(SIG_BLOCK, &mask, 0);

    win = gui_open("Park Test", 120, 120, 240, 140, 0);
    if (!win) {
        printf("parktest: no window manager\n");
        return 1;
    }
    paint(win, seed);
    gui_flush(win);
    if (background)
        gui_window_control(win->wid, K_WIN_CTL_MINIMIZE);
    win_sum = sum_words(win->px, win->w * win->h);
    xmm_set(XMM_MAGIC ^ seed);

    printf("PARKTEST READY pid=%d\n", pid);

    while (win->open) {
        int got = gui_next_event(win, &ev, 100);

        if (got < 0)
            break;
        counter++;
        if (getpid() == pid)
            continue;

        /* Resumed under a new pid: everything must be as we left it. */
        {
            const char *bad = 0;

            if (xmm_get() != (XMM_MAGIC ^ seed))
                bad = "sse register";
            else if (sum_words(heap, HEAP_WORDS) != heap_sum)
                bad = "heap";
            else if (sum_words(stack_pattern, STACK_WORDS) != stack_sum)
                bad = "stack";
            else if (sum_words(win->px, win->w * win->h) != win_sum)
                bad = "window pixels";
            else if (read(fd, buf, 6) != 6 || memcmp(buf, "line1\n", 6) != 0)
                bad = "file offset";
            else if (sigprocmask_(SIG_BLOCK, 0, &cur_mask) < 0 ||
                     !(cur_mask & mask))
                bad = "signal mask";
            if (bad)
                printf("PARKTEST RESUMED BAD %s\n", bad);
            else
                printf("PARKTEST RESUMED OK pid=%d counter=%lu\n",
                       getpid(), counter);
        }
        break;
    }
    gui_close(win);
    close(fd);
    return 0;
}
