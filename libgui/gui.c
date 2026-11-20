/* libgui: windows, the event loop, and the syscall wrappers the toolkit
 * needs. libc owns no window calls, so the thin int 0x80 wrappers for the
 * SYS_WIN_* family live here; everything above this file speaks gui_*.
 */

#include "gui.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define GUI_SETTINGS_PATH "/etc/desktop.conf"
#define GUI_THEME_PATH    "/etc/theme"

static gui_theme current_theme;
static gui_theme custom_theme;
static int has_custom_theme;
static gui_settings current_settings;
static int theme_ready;
static uint64_t theme_pulse_seq;
static int theme_pulse_inited;

static const char *settings_path(char path[64])
{
    int uid = gui_uid();

    if (uid <= 0)
        return GUI_SETTINGS_PATH;
    snprintf(path, 64, "/tmp/.kestrel-desktop-%d.conf", uid);
    return path;
}

static int setting_value(const char *text, const char *key, int fallback)
{
    unsigned long keylen = strlen(key);
    const char *p = text;

    while (p && *p) {
        const char *end = strchr(p, '\n');
        int sign = 1, value = 0, digits = 0;

        if (!end)
            end = p + strlen(p);
        if ((unsigned long)(end - p) > keylen &&
            !strncmp(p, key, keylen) && p[keylen] == '=') {
            const char *v = p + keylen + 1;

            if (v < end && *v == '-') {
                sign = -1;
                v++;
            }
            while (v < end && *v >= '0' && *v <= '9') {
                value = value * 10 + (*v - '0');
                digits = 1;
                v++;
            }
            if (digits)
                return value * sign;
            /* Allow named presets on preset= / theme=. */
            if (!strcmp(key, "preset")) {
                unsigned long vlen = (unsigned long)(end - v);
                while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' '))
                    vlen--;
                if (vlen == 4 && !strncmp(v, "dark", 4))
                    return GUI_THEME_AVIONICS_DARK;
                if (vlen == 8 && !strncmp(v, "avionics", 8))
                    return GUI_THEME_AVIONICS_DARK;
                if ((vlen == 8 && !strncmp(v, "daylight", 8)) ||
                    (vlen == 5 && !strncmp(v, "light", 5)))
                    return GUI_THEME_DAYLIGHT;
                if (vlen == 5 && !strncmp(v, "amber", 5))
                    return GUI_THEME_AMBER;
                if (vlen == 8 && !strncmp(v, "borealis", 8))
                    return GUI_THEME_BOREALIS;
                if (vlen == 6 && !strncmp(v, "custom", 6))
                    return GUI_THEME_CUSTOM;
            }
            return fallback;
        }
        p = *end ? end + 1 : end;
    }
    return fallback;
}

static void apply_theme(void)
{
    static const uint32_t accents[6] = {
        0x60A5FAU, 0x8B5CF6U, 0x2DD4BFU,
        0xFB7185U, 0xFBBF24U, 0x4ADE80U
    };
    int accent = current_settings.accent;
    int preset = current_settings.preset;
    int is_light = current_settings.light ? 1 : 0;

    if (accent < 0 || accent > 5)
        accent = 0;
    current_settings.accent = accent;
    if (preset < 0 || preset >= GUI_THEME_PRESET_COUNT)
        preset = is_light ? GUI_THEME_DAYLIGHT : GUI_THEME_AVIONICS_DARK;
    current_settings.preset = preset;

    if (preset == GUI_THEME_CUSTOM && has_custom_theme) {
        current_theme = custom_theme;
        return;
    }

    if (is_light || preset == GUI_THEME_DAYLIGHT) {
        /* Avionics Daylight: crisp instrument-paper light palette. */
        current_theme.desk_top = 0xE5ECF6U;
        current_theme.desk_bottom = 0xC4D2E6U;
        current_theme.panel = 0xF4F7FCU;
        current_theme.panel_hi = 0xFFFFFFU;
        current_theme.surface = 0xEDF2F8U;
        current_theme.surface_alt = 0xDFE7F2U;
        current_theme.sunken = 0xD5DFECU;
        current_theme.border = 0x94A4BCU;
        current_theme.edge = 0xBDCADDU;
        current_theme.text = 0x131C2EU;
        current_theme.text_dim = 0x5A6A82U;
        current_theme.text_inv = 0xF8FAFCU;
        current_theme.white = 0xFFFFFFU;
        current_theme.black = 0x0A101EU;
        current_theme.ok = 0x16A34AU;
        current_theme.warn = 0xD97706U;
        current_theme.error = 0xE11D48U;
        current_theme.accent = accents[accent];
        current_theme.accent_dark =
            gui_mix(accents[accent], 0x131C2EU, 92);
        return;
    }

    if (preset == GUI_THEME_AMBER) {
        /* Cockpit Amber: warm night-flight phosphor instrumentation. */
        current_theme.desk_top = 0x16110AU;
        current_theme.desk_bottom = 0x090603U;
        current_theme.panel = 0x1C160EU;
        current_theme.panel_hi = 0x2D2316U;
        current_theme.surface = 0x211A10U;
        current_theme.surface_alt = 0x2A2115U;
        current_theme.sunken = 0x120D08U;
        current_theme.border = 0x0B0804U;
        current_theme.edge = 0x544128U;
        current_theme.text = 0xFFF1D6U;
        current_theme.text_dim = 0xB89B6EU;
        current_theme.text_inv = 0x140E07U;
        current_theme.white = 0xFFFDF8U;
        current_theme.black = 0x060402U;
    } else if (preset == GUI_THEME_BOREALIS) {
        /* Borealis: cool polar emerald-teal avionics slate. */
        current_theme.desk_top = 0x0A1A1FU;
        current_theme.desk_bottom = 0x040B0EU;
        current_theme.panel = 0x0E2228U;
        current_theme.panel_hi = 0x183740U;
        current_theme.surface = 0x11282FU;
        current_theme.surface_alt = 0x18353EU;
        current_theme.sunken = 0x08161BU;
        current_theme.border = 0x050E12U;
        current_theme.edge = 0x295663U;
        current_theme.text = 0xE6F7F7U;
        current_theme.text_dim = 0x7EB3BAU;
        current_theme.text_inv = 0x08161BU;
        current_theme.white = 0xF4FFFFU;
        current_theme.black = 0x03080AU;
    } else {
        /* Avionics Dark: deep obsidian-slate flight deck. */
        current_theme.desk_top = 0x0C1320U;
        current_theme.desk_bottom = 0x05080EU;
        current_theme.panel = 0x111A28U;
        current_theme.panel_hi = 0x1E2C42U;
        current_theme.surface = 0x152031U;
        current_theme.surface_alt = 0x1C2A40U;
        current_theme.sunken = 0x0B111BU;
        current_theme.border = 0x080D15U;
        current_theme.edge = 0x344763U;
        current_theme.text = 0xEDF3FAU;
        current_theme.text_dim = 0x8FA1BAU;
        current_theme.text_inv = 0x0C1320U;
        current_theme.white = 0xF8FAFCU;
        current_theme.black = 0x04070CU;
    }
    current_theme.accent = accents[accent];
    current_theme.accent_dark =
        gui_mix(accents[accent], current_theme.black, 94);
    current_theme.ok = preset == GUI_THEME_BOREALIS ? 0x34D399U : 0x4ADE80U;
    current_theme.warn = 0xFBBF24U;
    current_theme.error = 0xFB7185U;
}

const gui_theme *gui_theme_current(void)
{
    if (!theme_ready) {
        memset(&current_settings, 0, sizeof(current_settings));
        apply_theme();
        theme_ready = 1;
    }
    return &current_theme;
}

void gui_settings_get(gui_settings *out)
{
    (void)gui_theme_current();
    if (out)
        *out = current_settings;
}

void gui_settings_reload(void)
{
    gui_settings next;
    char path[64];
    char buf[512];
    long n;
    int fd;
    int raw_preset;

    memset(&next, 0, sizeof(next));
    fd = open(settings_path(path), O_RDONLY);
    if (fd < 0 && gui_uid() > 0)
        fd = open(GUI_SETTINGS_PATH, O_RDONLY);
    if (fd < 0)
        fd = open(GUI_THEME_PATH, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            next.light = setting_value(buf, "light", 0);
            next.accent = setting_value(buf, "accent", 0);
            next.wallpaper = setting_value(buf, "wallpaper", 0);
            next.compact = setting_value(buf, "compact", 0);
            raw_preset = setting_value(buf, "preset", -1);
            if (raw_preset < 0)
                raw_preset = next.light ? GUI_THEME_DAYLIGHT
                                        : GUI_THEME_AVIONICS_DARK;
            next.preset = raw_preset;
        }
    }
    next.light = next.light ? 1 : 0;
    if (next.accent < 0 || next.accent > 5)
        next.accent = 0;
    if (next.wallpaper < 0 || next.wallpaper > 3)
        next.wallpaper = 0;
    next.compact = next.compact ? 1 : 0;
    if (next.preset < 0 || next.preset >= GUI_THEME_PRESET_COUNT)
        next.preset = next.light ? GUI_THEME_DAYLIGHT
                                 : GUI_THEME_AVIONICS_DARK;
    current_settings = next;
    apply_theme();
    theme_ready = 1;
}

int gui_theme_sync_wm(void)
{
    const gui_theme *th = gui_theme_current();
    struct k_wm_theme wt;

    wt.desktop_bg = th->desk_bottom;
    wt.title_active = gui_mix(th->panel, th->panel_hi, 176);
    wt.title_inactive = th->panel;
    wt.border_active = th->edge;
    wt.border_inactive = th->border;
    wt.accent = th->accent;
    wt.text_active = th->text;
    wt.text_inactive = th->text_dim;
    wt.btn_bg = th->surface_alt;
    wt.close_bg = th->error;
    wt.close_fg = th->white;
    wt._pad = 0;
    return wm_theme(&wt);
}

int gui_theme_set_preset(int preset)
{
    if (preset < 0 || preset >= GUI_THEME_PRESET_COUNT)
        return -1;
    (void)gui_theme_current();
    current_settings.preset = preset;
    current_settings.light = (preset == GUI_THEME_DAYLIGHT) ? 1 : 0;
    if (preset == GUI_THEME_AMBER)
        current_settings.accent = 4;
    else if (preset == GUI_THEME_BOREALIS)
        current_settings.accent = 2;
    else if (preset == GUI_THEME_AVIONICS_DARK ||
             preset == GUI_THEME_DAYLIGHT)
        current_settings.accent = 0;
    apply_theme();
    theme_ready = 1;
    (void)gui_theme_sync_wm();
    return 0;
}

int gui_theme_get_preset(void)
{
    (void)gui_theme_current();
    if (current_settings.light &&
        current_settings.preset != GUI_THEME_CUSTOM)
        return GUI_THEME_DAYLIGHT;
    return current_settings.preset;
}

void gui_theme_set_custom(const gui_theme *custom)
{
    if (!custom)
        return;
    (void)gui_theme_current();
    custom_theme = *custom;
    has_custom_theme = 1;
    current_settings.preset = GUI_THEME_CUSTOM;
    apply_theme();
    theme_ready = 1;
    (void)gui_theme_sync_wm();
}

int gui_settings_save(const gui_settings *settings)
{
    char path[64];
    char buf[256];
    int len, fd;
    long n;

    if (!settings)
        return -1;
    len = snprintf(buf, sizeof(buf),
                   "# Kestrel desktop appearance\n"
                   "light=%d\naccent=%d\nwallpaper=%d\ncompact=%d\npreset=%d\n",
                   settings->light ? 1 : 0, settings->accent,
                   settings->wallpaper, settings->compact ? 1 : 0,
                   settings->preset);
    if (len <= 0 || len >= (int)sizeof(buf))
        return -1;
    fd = open(settings_path(path), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    n = write(fd, buf, (unsigned long)len);
    close(fd);
    if (n != len)
        return -1;
    gui_settings_reload();
    (void)gui_theme_sync_wm();
    return 0;
}

static void theme_pulse_init(void)
{
    struct k_pulse_query q;

    q.seq = 0;
    q.topic_mask = K_TOPIC_THEME;
    q.max_out = 0;
    q.timeout_ms = 0;
    q._pad = 0;
    if (pulse_poll(&q, 0) >= 0)
        theme_pulse_seq = q.seq;
    theme_pulse_inited = 1;
}

static int theme_pulse_check(void)
{
    struct k_pulse_query q;
    struct k_pulse pulses[4];
    int changed = 0;

    if (!theme_pulse_inited) {
        theme_pulse_init();
        return 0;
    }
    for (;;) {
        int n;

        q.seq = theme_pulse_seq;
        q.topic_mask = K_TOPIC_THEME;
        q.max_out = 4;
        q.timeout_ms = 0;
        q._pad = 0;
        n = pulse_poll(&q, pulses);
        if (n <= 0) {
            if (n == 0)
                theme_pulse_seq = q.seq;
            break;
        }
        theme_pulse_seq = q.seq;
        changed = 1;
        if (n < 4)
            break;
    }
    if (changed)
        gui_settings_reload();
    return changed;
}

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

static long sys_win_list(int index, struct k_winsummary *out)
{
    return syscall(SYS_WIN_LIST, index, (long)out, 0, 0);
}

static long sys_win_ctl(uint32_t wid, int action)
{
    return syscall(SYS_WIN_CTL, (long)wid, action, 0, 0);
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
    if (pid <= 0)
        return -1;
    return (int)syscall(SYS_KILL, pid, SIGKILL, 0, 0);
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
    gui_settings_reload();
    theme_pulse_init();
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
    memset(win, 0, sizeof(*win));
    win->wid = info.wid;
    win->w = (int)info.width;
    win->h = (int)info.height;
    win->px = (uint32_t *)(unsigned long)info.buffer;
    win->open = 1;
    win->clip_depth = 0;
    win->clip = gui_mkrc(0, 0, win->w, win->h);

    if (flags & K_WIN_DESKTOP) {
        (void)gui_theme_sync_wm();
        theme_pulse_init();
    }

    gui_clear(win, GUI_SURFACE);
    return win;
}

void gui_close(gui_window *win)
{
    if (!win || win->row_stride)
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

void gui_window_resized(gui_window *win, int w, int h)
{
    if (!win || win->row_stride || w <= 0 || h <= 0)
        return;
    win->w = w;
    win->h = h;
    win->clip_depth = 0;
    win->clip = gui_mkrc(0, 0, w, h);
}

int gui_resize(gui_window *win, int w, int h)
{
    if (!win || !win->open || w <= 0 || h <= 0)
        return -1;
    if (win_resize(win->wid, w, h) != 0)
        return -1;
    gui_window_resized(win, w, h);
    return 0;
}

int gui_window_list(int index, struct k_winsummary *out)
{
    if (index < 0 || !out)
        return -1;
    return sys_win_list(index, out) == 0 ? 0 : -1;
}

int gui_window_control(uint32_t wid, int action)
{
    return sys_win_ctl(wid, action) == 0 ? 0 : -1;
}

int gui_next_event(gui_window *win, struct k_event *ev, int timeout_ms)
{
    long r;
    int reloaded_before;

    if (!win || !win->open || !ev)
        return -1;
    reloaded_before = theme_pulse_check();
    memset(ev, 0, sizeof(*ev));
    r = sys_win_event(win->wid, ev, timeout_ms);
    if (r < 0) {
        /* The window vanished (compositor gone, killed by the wm). Report
         * it as fatal so callers exit instead of spinning on -1. */
        win->open = 0;
        return -1;
    }
    if (r == 0) {
        (void)theme_pulse_check();
        return 0;
    }
    if (ev->type == KEV_CLOSE)
        win->open = 0;
    else if (ev->type == KEV_RESIZE && ev->x > 0 && ev->y > 0)
        gui_window_resized(win, ev->x, ev->y);
    else if (ev->type == KEV_THEME) {
        if (!theme_pulse_check() && !reloaded_before)
            gui_settings_reload();
    }
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
