/* about.c - the system information window.
 *
 * A drawn emblem, the release string from /etc/version, and the live
 * numbers: uptime, memory (with a usage bar), process count, the user
 * this window belongs to, the screen mode and the network configuration.
 * Everything refreshes once a second.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define WIN_W    540
#define WIN_H    412
#define EMBLEM_X 92
#define EMBLEM_Y 116
#define EMBLEM_R 66
#define INFO_X   190
#define INFO_Y   40
#define ROW_H    26

static char version[64] = "KestrelOS";

static void read_version(void)
{
    char buf[64];
    char *nl;
    long n;
    int fd = open("/etc/version", O_RDONLY);

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    if (buf[0])
        snprintf(version, sizeof(version), "%s", buf);
}

/* A drawn mark rather than an imported image: a badge with a stylized
 * kestrel - swept wings, a body and a fanned tail - all straight lines. */
static void draw_emblem(gui_window *win)
{
    int cx = EMBLEM_X, cy = EMBLEM_Y;
    int i;

    gui_disc(win, cx, cy, EMBLEM_R + 4, gui_shade(GUI_SURFACE, -18));
    gui_disc(win, cx, cy, EMBLEM_R, GUI_ACCENT_DARK);
    for (i = 0; i < EMBLEM_R; i++)
        gui_circle(win, cx, cy, EMBLEM_R - i,
                   gui_mix(GUI_ACCENT_DARK, GUI_DESK_TOP, i * 3));
    gui_circle(win, cx, cy, EMBLEM_R, gui_shade(GUI_ACCENT, 20));

    /* wings */
    gui_line_w(win, cx, cy - 6, cx - 46, cy - 32, GUI_WHITE, 3);
    gui_line_w(win, cx - 46, cy - 32, cx - 26, cy - 4, GUI_WHITE, 3);
    gui_line_w(win, cx, cy - 6, cx + 46, cy - 32, GUI_WHITE, 3);
    gui_line_w(win, cx + 46, cy - 32, cx + 26, cy - 4, GUI_WHITE, 3);
    /* body and head */
    gui_line_w(win, cx, cy - 16, cx, cy + 26, GUI_WHITE, 4);
    gui_disc(win, cx, cy - 20, 5, GUI_WHITE);
    gui_line_w(win, cx + 4, cy - 21, cx + 12, cy - 19, GUI_WARN, 2);
    /* fanned tail */
    gui_line_w(win, cx, cy + 24, cx - 12, cy + 40, GUI_WHITE, 2);
    gui_line_w(win, cx, cy + 24, cx, cy + 44, GUI_WHITE, 2);
    gui_line_w(win, cx, cy + 24, cx + 12, cy + 40, GUI_WHITE, 2);
}

static int row(gui_window *win, int y, const char *label, const char *value)
{
    gui_text(win, INFO_X, y, label, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_text(win, INFO_X + 88, y, value, GUI_TEXT, GUI_TRANSPARENT);
    return y + ROW_H;
}

static void draw_meter(gui_window *win, int x, int y, int w, int h,
                       int pct, uint32_t fill)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    gui_rect(win, x, y, w, h, GUI_SUNKEN);
    gui_rect(win, x + 1, y + 1, ((w - 2) * pct) / 100, h - 2, fill);
    gui_frame(win, x, y, w, h, GUI_BORDER);
}

static void paint(gui_window *win, gui_ui *ui, int *quit)
{
    char buf[96], buf2[32];
    struct k_netinfo ni;
    struct k_psinfo ps;
    struct k_fbinfo fb;
    struct gui_tm tm;
    uint64_t total_kb = 0, free_kb = 0;
    unsigned long secs;
    int y = INFO_Y, nproc = 0, pct = 0;

    gui_clear(win, GUI_SURFACE);
    gui_vgradient(win, gui_mkrc(0, 0, WIN_W, 34), gui_shade(GUI_PANEL, 10),
                  GUI_PANEL);
    gui_text(win, 12, 9, "About this system", GUI_TEXT, GUI_TRANSPARENT);
    gui_rect(win, 0, 34, WIN_W, 1, GUI_BORDER);

    draw_emblem(win);
    gui_text(win, EMBLEM_X - gui_text_w("KestrelOS") / 2,
             EMBLEM_Y + EMBLEM_R + 22, "KestrelOS", GUI_ACCENT,
             GUI_TRANSPARENT);

    y = row(win, y + 14, "release", version);
    y = row(win, y, "kernel", "kestrel x86_64 (long mode)");

    secs = uptime_ms() / 1000;
    snprintf(buf, sizeof(buf), "%luh %02lum %02lus", secs / 3600,
             (secs / 60) % 60, secs % 60);
    y = row(win, y, "uptime", buf);

    if (meminfo(&total_kb, &free_kb) == 0 && total_kb > 0) {
        uint64_t used = total_kb - free_kb;

        pct = (int)((used * 100) / total_kb);
        snprintf(buf, sizeof(buf), "%llu / %llu MiB  (%d%%)",
                 (unsigned long long)(used / 1024),
                 (unsigned long long)(total_kb / 1024), pct);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = row(win, y, "memory", buf);
    draw_meter(win, INFO_X + 88, y - 6, 220, 10, pct,
               pct > 85 ? GUI_ERROR : GUI_OK);
    y += 12;

    while (psinfo(nproc, &ps) == 0)
        nproc++;
    snprintf(buf, sizeof(buf), "%d running", nproc);
    y = row(win, y, "processes", buf);

    snprintf(buf, sizeof(buf), "uid %d", gui_uid());
    y = row(win, y, "user", buf);

    memset(&fb, 0, sizeof(fb));
    if (syscall(SYS_FBINFO, (long)&fb, 0, 0, 0) == 0 && fb.present)
        snprintf(buf, sizeof(buf), "%ux%u  %u bpp", fb.width, fb.height,
                 fb.bpp);
    else
        snprintf(buf, sizeof(buf), "text mode");
    y = row(win, y, "display", buf);

    if (netinfo(&ni) == 0 && ni.up) {
        ip_ntoa(ni.ip, buf2);
        snprintf(buf, sizeof(buf), "%s", buf2);
        y = row(win, y, "address", buf);
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4],
                 ni.mac[5]);
        y = row(win, y, "mac", buf);
    } else {
        y = row(win, y, "network", "down");
        y += ROW_H;
    }

    gui_gmtime(gui_time(), &tm);
    snprintf(buf, sizeof(buf), "%s %d %s %d  %02d:%02d:%02d UTC",
             gui_day_name(tm.wday), tm.day, gui_month_name(tm.mon),
             tm.year, tm.hour, tm.min, tm.sec);
    gui_text(win, 14, WIN_H - 30 - GUI_FONT_H - 6, buf, GUI_TEXT_DIM,
             GUI_TRANSPARENT);

    if (gui_button(win, gui_mkrc(WIN_W - 100, WIN_H - 38, 88, 28), "Close",
                   ui))
        *quit = 1;
}

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *win;
    gui_ui ui;
    unsigned long next = 0;
    int quit = 0, dirty = 1;

    (void)argc;
    (void)argv;

    read_version();
    memset(&ui, 0, sizeof(ui));

    win = gui_open("About KestrelOS", 200, 130, WIN_W, WIN_H, 0);
    if (!win) {
        printf("about: no window manager available\n");
        return 1;
    }

    while (win->open && !quit) {
        int got = gui_next_event(win, &ev, 100);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && (ev.key == 27 || ev.key == 'q'))
                break;
            dirty = 1;
        }
        if (uptime_ms() >= next) {
            next = uptime_ms() + 1000;
            dirty = 1;
        }
        if (!dirty)
            continue;
        paint(win, &ui, &quit);
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_close(win);
    return 0;
}
