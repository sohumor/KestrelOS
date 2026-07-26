/* libgui: windows, the event loop, and the syscall wrappers the toolkit
 * needs. libc owns no window calls, so the thin int 0x80 wrappers for the
 * SYS_WIN_* family live here; everything above this file speaks gui_*.
 */

#include "gui.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ syscalls */

static long sys_fbinfo(struct k_fbinfo *fb)
{
    return syscall(SYS_FBINFO, (long)fb, 0, 0, 0);
}

static long sys_win_create(const struct k_wincreate *req,
                           struct k_wininfo *out)
{
    return syscall(SYS_WIN_CREATE, (long)req, (long)out, 0, 0);
}

static long sys_win_destroy(uint32_t wid)
{
    return syscall(SYS_WIN_DESTROY, (long)wid, 0, 0, 0);
}

static long sys_win_flush(uint32_t wid)
{
    return syscall(SYS_WIN_FLUSH, (long)wid, 0, 0, 0);
}

static long sys_win_event(uint32_t wid, struct k_event *ev, int timeout_ms)
{
    return syscall(SYS_WIN_EVENT, (long)wid, (long)ev, timeout_ms, 0);
}

static long sys_win_move(uint32_t wid, int x, int y)
{
    return syscall(SYS_WIN_MOVE, (long)wid, x, y, 0);
}

unsigned long gui_time(void)
{
    return (unsigned long)syscall(SYS_TIME, 0, 0, 0, 0);
}

int gui_uid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

int gui_kill(int pid)
{
    return (int)syscall(SYS_KILL, pid, 0, 0, 0);
}

int gui_mouse(struct k_mouse *out)
{
    return (int)syscall(SYS_MOUSE, (long)out, 0, 0, 0);
}

/* -------------------------------------------------------------- windows */

int gui_screen(int *w, int *h)
{
    struct k_fbinfo fb;

    memset(&fb, 0, sizeof(fb));
    if (sys_fbinfo(&fb) != 0 || !fb.present || fb.width == 0)
        return -1;
    if (w)
        *w = (int)fb.width;
    if (h)
        *h = (int)fb.height;
    return 0;
}

gui_window *gui_open(const char *title, int x, int y, int w, int h,
                     unsigned flags)
{
    struct k_wincreate req;
    struct k_wininfo info;
    gui_window *win;

    if (w <= 0 || h <= 0)
        return 0;
    /* No framebuffer means no compositor: fail here rather than let the
     * caller block forever waiting for events that can never arrive. */
    if (gui_screen(0, 0) != 0)
        return 0;

    memset(&req, 0, sizeof(req));
    memset(&info, 0, sizeof(info));
    req.x = x;
    req.y = y;
    req.width = (uint32_t)w;
    req.height = (uint32_t)h;
    req.flags = flags;
    if (title) {
        strncpy(req.title, title, sizeof(req.title) - 1);
        req.title[sizeof(req.title) - 1] = '\0';
    }

    if (sys_win_create(&req, &info) != 0)
        return 0;
    if (info.buffer == 0 || info.width == 0 || info.height == 0) {
        sys_win_destroy(info.wid);
        return 0;
    }

    win = malloc(sizeof(*win));
    if (!win) {
        sys_win_destroy(info.wid);
        return 0;
    }
    win->wid = info.wid;
    win->w = (int)info.width;
    win->h = (int)info.height;
    win->px = (uint32_t *)(unsigned long)info.buffer;
    win->open = 1;
    win->clip = gui_mkrc(0, 0, win->w, win->h);

    gui_clear(win, GUI_SURFACE);
    return win;
}

void gui_close(gui_window *win)
{
    if (!win)
        return;
    /* Always ask the compositor to drop it, even when win->open was
     * already cleared by KEV_CLOSE: the close box is a request, and the
     * window only really goes away when its owner destroys it. A wid the
     * compositor has forgotten simply returns -1. */
    sys_win_destroy(win->wid);
    win->open = 0;
    win->px = 0;
    free(win);
}

int gui_flush(gui_window *win)
{
    if (!win || !win->open)
        return -1;
    if (sys_win_flush(win->wid) != 0) {
        win->open = 0;
        return -1;
    }
    return 0;
}

int gui_move(gui_window *win, int x, int y)
{
    if (!win || !win->open)
        return -1;
    return sys_win_move(win->wid, x, y) == 0 ? 0 : -1;
}

int gui_next_event(gui_window *win, struct k_event *ev, int timeout_ms)
{
    long r;

    if (!win || !win->open || !ev)
        return -1;
    memset(ev, 0, sizeof(*ev));
    r = sys_win_event(win->wid, ev, timeout_ms);
    if (r < 0) {
        /* The window vanished (compositor gone, killed by the wm). Report
         * it as fatal so callers exit instead of spinning on -1. */
        win->open = 0;
        return -1;
    }
    if (r == 0)
        return 0;
    if (ev->type == KEV_CLOSE)
        win->open = 0;
    return 1;
}

int gui_run(gui_window *win, gui_handler fn, void *ctx, int tick_ms)
{
    struct k_event ev;
    int rc = 0;

    if (!win || !fn)
        return -1;
    if (tick_ms <= 0)
        tick_ms = 50;

    while (win->open) {
        int got = gui_next_event(win, &ev, tick_ms);

        if (got < 0)
            return -1;
        if (got == 0) {
            memset(&ev, 0, sizeof(ev));
            ev.type = KEV_NONE;
        }
        rc = fn(win, &ev, ctx);
        if (rc != 0)
            break;
        if (ev.type == KEV_CLOSE)
            break;
    }
    return rc;
}

/* ------------------------------------------------------------ calendar */

static int leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int y, int m)
{
    static const int len[12] = { 31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31 };

    if (m == 2 && leap_year(y))
        return 29;
    return len[m - 1];
}

/* Straight subtraction: peel whole years off the day count, then whole
 * months. At a few thousand iterations worst case this is far cheaper
 * than anything that needs division tables, and it is obviously right. */
void gui_gmtime(unsigned long secs, struct gui_tm *tm)
{
    unsigned long days = secs / 86400UL;
    unsigned long rem = secs % 86400UL;
    int year = 1970;
    int mon = 1;

    if (!tm)
        return;
    tm->hour = (int)(rem / 3600UL);
    tm->min = (int)((rem / 60UL) % 60UL);
    tm->sec = (int)(rem % 60UL);
    /* 1970-01-01 was a Thursday. */
    tm->wday = (int)((days + 4UL) % 7UL);

    for (;;) {
        unsigned long len = leap_year(year) ? 366UL : 365UL;

        if (days < len)
            break;
        days -= len;
        year++;
    }
    tm->year = year;
    tm->yday = (int)days;

    while (days >= (unsigned long)month_days(year, mon)) {
        days -= (unsigned long)month_days(year, mon);
        mon++;
    }
    tm->mon = mon;
    tm->day = (int)days + 1;
}

const char *gui_month_name(int mon)
{
    static const char *names[12] = { "Jan", "Feb", "Mar", "Apr", "May",
                                     "Jun", "Jul", "Aug", "Sep", "Oct",
                                     "Nov", "Dec" };

    if (mon < 1 || mon > 12)
        return "???";
    return names[mon - 1];
}

const char *gui_day_name(int wday)
{
    static const char *names[7] = { "Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat" };

    if (wday < 0 || wday > 6)
        return "???";
    return names[wday];
}
