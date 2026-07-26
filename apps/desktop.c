/* desktop.c - the KestrelOS desktop shell.
 *
 * One full-screen K_WIN_DESKTOP window that the compositor keeps behind
 * everything else: a computed background gradient with the wordmark, and
 * a taskbar carrying the launcher buttons and a clock. Clicking a
 * launcher spawns the program; the desktop reaps its children as they
 * exit so they do not pile up as zombies.
 *
 * The background is painted once (it is a megapixel of arithmetic) and
 * only the taskbar strip is repainted afterwards.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define BAR_H        38
#define BTN_W        104
#define BTN_H        26
#define MAX_CHILDREN 32
#define TAGLINE "a small operating system, written from scratch"

struct launcher {
    const char *label;
    const char *path;
    int enabled;
};

static struct launcher apps[] = {
    { "Terminal", "/bin/terminal", 0 },
    { "Files",    "/bin/gfiles",   0 },
    { "Clock",    "/bin/gclock",   0 },
    { "Paint",    "/bin/gpaint",   0 },
    { "About",    "/bin/about",    0 },
    { "Browser",  "/bin/browser",  0 },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

static int children[MAX_CHILDREN];
static int nchildren;
static char message[80];
static unsigned long message_until;

/* ---------------------------------------------------------- background */

/* Draw text with every font pixel expanded to a scale*scale block: the
 * only way to get a large wordmark out of a single 8x16 face. */
static void text_scaled(gui_window *win, int x, int y, const char *s,
                        int scale, uint32_t color)
{
    int row, col;

    for (; *s; s++) {
        const uint8_t *g = gui_glyph((unsigned char)*s);

        for (row = 0; row < GUI_FONT_H; row++) {
            for (col = 0; col < GUI_FONT_W; col++) {
                if (!(g[row] & (0x80 >> col)))
                    continue;
                gui_rect(win, x + col * scale, y + row * scale, scale,
                         scale, color);
            }
        }
        x += GUI_FONT_W * scale;
    }
}

static void paint_background(gui_window *win)
{
    int h = win->h - BAR_H;
    int i, wordmark_w;

    gui_vgradient(win, gui_mkrc(0, 0, win->w, h), GUI_DESK_TOP,
                  GUI_DESK_BOTTOM);

    /* A few slack diagonals for texture: cheap, subtle, and nothing like
     * a photograph nobody can ship in a disk image. */
    for (i = 0; i < 6; i++) {
        int y0 = h / 3 + i * 26;

        gui_line(win, 0, y0, win->w, y0 - win->w / 3,
                 gui_mix(GUI_DESK_TOP, GUI_ACCENT, 12 + i));
    }

    wordmark_w = gui_text_w("KestrelOS") * 4;
    text_scaled(win, (win->w - wordmark_w) / 2, h / 2 - 60, "KestrelOS", 4,
                gui_mix(GUI_DESK_TOP, GUI_ACCENT, 90));
    gui_text(win, (win->w - gui_text_w(TAGLINE)) / 2, h / 2 + 16, TAGLINE,
             gui_mix(GUI_DESK_TOP, GUI_TEXT, 120), GUI_TRANSPARENT);
}

/* -------------------------------------------------------------- taskbar */

static gui_rc button_rc(int index, int bar_y)
{
    return gui_mkrc(10 + index * (BTN_W + 6), bar_y + (BAR_H - BTN_H) / 2,
                    BTN_W, BTN_H);
}

static void clock_text(char *buf, unsigned long size)
{
    struct gui_tm tm;
    unsigned long now = gui_time();

    if (now == 0 || (long)now < 0) {
        snprintf(buf, size, "--:--");
        return;
    }
    gui_gmtime(now, &tm);
    snprintf(buf, size, "%s %d %s  %02d:%02d", gui_day_name(tm.wday),
             tm.day, gui_month_name(tm.mon), tm.hour, tm.min);
}

static void paint_bar(gui_window *win, gui_ui *ui, int *launch)
{
    int bar_y = win->h - BAR_H;
    char clk[48];
    int i, tw;

    gui_vgradient(win, gui_mkrc(0, bar_y, win->w, BAR_H),
                  gui_shade(GUI_PANEL, 8), gui_shade(GUI_PANEL, -16));
    gui_rect(win, 0, bar_y, win->w, 1, GUI_EDGE);

    for (i = 0; i < NAPPS; i++) {
        gui_rc r = button_rc(i, bar_y);

        if (gui_button_ex(win, r, apps[i].label, ui, apps[i].enabled, 0))
            *launch = i;
    }

    clock_text(clk, sizeof(clk));
    tw = gui_text_w(clk);
    gui_rect(win, win->w - tw - 24, bar_y + 6, tw + 16, BAR_H - 12,
             gui_shade(GUI_PANEL, -22));
    gui_frame(win, win->w - tw - 24, bar_y + 6, tw + 16, BAR_H - 12,
              GUI_BORDER);
    gui_text(win, win->w - tw - 16, bar_y + (BAR_H - GUI_FONT_H) / 2, clk,
             GUI_TEXT, GUI_TRANSPARENT);

    if (message[0] && uptime_ms() < message_until) {
        int x = 10 + NAPPS * (BTN_W + 6) + 16;

        gui_clip(win, gui_mkrc(x, bar_y, win->w - tw - 40 - x, BAR_H));
        gui_text(win, x, bar_y + (BAR_H - GUI_FONT_H) / 2, message,
                 GUI_WARN, GUI_TRANSPARENT);
        gui_unclip(win);
    } else {
        message[0] = '\0';
    }
}

/* -------------------------------------------------------------- children */

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_until = uptime_ms() + 4000;
}

static void launch(int index)
{
    char *argv[2];
    int pid;

    argv[0] = (char *)apps[index].path;
    argv[1] = 0;
    pid = spawn(apps[index].path, argv);
    if (pid < 0) {
        note("cannot start %s", apps[index].path);
        return;
    }
    if (nchildren < MAX_CHILDREN)
        children[nchildren++] = pid;
}

/* Reap without blocking: waitpid() only after psinfo says the child has
 * become a zombie, so the desktop never stalls on a running program. */
static void reap(void)
{
    struct k_psinfo ps;
    int i, j, state;

    for (i = 0; i < nchildren; i++) {
        int found = 0;

        state = -1;
        j = 0;
        while (psinfo(j++, &ps) == 0) {
            if (ps.pid == children[i]) {
                found = 1;
                state = ps.state;
                break;
            }
        }
        if (found && state != K_STATE_ZOMBIE)
            continue;
        if (found)
            waitpid(children[i]);
        for (j = i + 1; j < nchildren; j++)
            children[j - 1] = children[j];
        nchildren--;
        i--;
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *win;
    gui_ui ui;
    struct k_stat st;
    int sw, sh, i, dirty = 1;
    unsigned long next_tick = 0;

    (void)argc;
    (void)argv;

    if (gui_screen(&sw, &sh) != 0) {
        printf("desktop: no framebuffer, nothing to draw on\n");
        return 1;
    }
    win = gui_open("Desktop", 0, 0, sw, sh,
                   K_WIN_DESKTOP | K_WIN_NODECOR);
    if (!win) {
        printf("desktop: no window manager available\n");
        return 1;
    }

    for (i = 0; i < NAPPS; i++)
        apps[i].enabled = stat_(apps[i].path, &st) == 0 && !st.is_dir;

    memset(&ui, 0, sizeof(ui));
    paint_background(win);

    while (win->open) {
        int got = gui_next_event(win, &ev, 100);
        int want = -1;

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            dirty = 1;
        }
        if (uptime_ms() >= next_tick) {
            next_tick = uptime_ms() + 1000;
            reap();
            dirty = 1;
        }
        if (!dirty)
            continue;

        paint_bar(win, &ui, &want);
        if (want >= 0)
            launch(want);
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_close(win);
    return 0;
}
