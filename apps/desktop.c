/* desktop.c - the KestrelOS Avionics desktop environment.
 *
 * Full-screen K_WIN_DESKTOP shell owning the wallpaper, Top Telemetry Panel
 * (Perch trigger, Wind-Hover Workspaces 1..4, active-workspace task pills,
 * Talon Shelf badge, live CPU/RAM sparklines, network & clock), bottom
 * quick-launch dock, toast notifications, and undecorated overlay popups for
 * Kestrel Perch, Talon Visual Shelf, and Quick Settings.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define TOP_H          34
#define DOCK_H_FULL    60
#define DOCK_H_COMPACT 50
#define MAX_CHILDREN   32
#define MAX_TASKS      16
#define SPARK_SAMPLES  24

#define LAUNCH_W       660
#define LAUNCH_H       480
#define TALON_W        476
#define TALON_H        496
#define QUICK_W        396
#define QUICK_H        382
#define HANGAR_W       588
#define HANGAR_H       468

#define CALC_LONG_MAX  __LONG_MAX__
#define CALC_LONG_MIN  (-__LONG_MAX__ - 1L)

enum popup_kind {
    POP_NONE = 0,
    POP_LAUNCHER,
    POP_TALON,
    POP_QUICK,
    POP_HANGAR
};

struct app_entry {
    const char *name;
    const char *desc;
    const char *path;
    const char *arg;
    enum gui_icon icon;
    uint32_t color;
    int enabled;
    int pinned_dock;
};

static struct app_entry apps[] = {
    { "Sight", "Kernel, proc & IPC inspector", "/bin/sight", 0,
      GUI_ICON_SEARCH, 0x06B6D4U, 0, 1 },
    { "Notes", "Quick notes & Talon pad", "/bin/notes", 0,
      GUI_ICON_INFO, 0xFBBF24U, 0, 1 },
    { "KJS Demo", "Flight HUD JavaScript demo", "/bin/kjs",
      "/doc/demos/flight_hud.js", GUI_ICON_TERMINAL, 0xA855F7U, 0, 0 },
    { "Editor", "Text and code editor", "/bin/edit", 0,
      GUI_ICON_PAINT, 0x10B981U, 0, 1 },
    { "Browser", "Explore web and local docs", "/bin/browser", 0,
      GUI_ICON_BROWSER, 0x3B82F6U, 0, 1 },
    { "Terminal", "Command line and shell", "/bin/terminal", 0,
      GUI_ICON_TERMINAL, 0x475569U, 0, 1 },
    { "Files", "Browse files and folders", "/bin/gfiles", 0,
      GUI_ICON_FILES, 0xF59E0BU, 0, 1 },
    { "System Monitor", "Telemetry, procs & pulses", "/bin/sysmon", 0,
      GUI_ICON_MONITOR, 0x22C55EU, 0, 1 },
    { "Paint", "Draw and edit KPX images", "/bin/gpaint", 0,
      GUI_ICON_PAINT, 0xEC4899U, 0, 0 },
    { "Calculator", "64-bit desktop calculator", "/bin/gcalc", 0,
      GUI_ICON_CALCULATOR, 0x14B8A6U, 0, 0 },
    { "Clock", "UTC dial and calendar", "/bin/gclock", 0,
      GUI_ICON_CLOCK, 0x8B5CF6U, 0, 0 },
    { "Settings", "Avionics themes & workspaces", "/bin/settings", 0,
      GUI_ICON_SETTINGS, 0x64748BU, 0, 1 },
    { "About", "KestrelOS system information", "/bin/about", 0,
      GUI_ICON_INFO, 0x0EA5E9U, 0, 0 },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

struct talon_slot {
    struct k_parcel_info info;
    char preview[96];
    int has_color;
    uint32_t color_rgb;
};

struct perch_eval {
    int valid;
    int is_error;
    uint32_t parcel_type;
    char title[72];
    char subtitle[96];
    char copy_text[96];
    char copy_label[48];
    int has_color;
    uint32_t color_rgb;
};

static int children[MAX_CHILDREN];
static int nchildren;
static char toast[192];
static unsigned long toast_until;
static int toast_needs_background;

/* Shell live state updated by kernel pulses & periodic telemetry tick. */
static struct k_winsummary tasks[MAX_TASKS];
static int ntasks;
static int active_ws;
static struct k_netinfo net_cache;
static int net_online;
static uint64_t pulse_seq;

static int cpu_hist[SPARK_SAMPLES];
static int ram_hist[SPARK_SAMPLES];
static int last_cpu_pct;
static int last_ram_pct;
static uint32_t pulse_burst;

static struct talon_slot talon_slots[K_PARCEL_MAX_SLOTS];
static int ntalon;

/* Popup overlay state */
static gui_window *popup;
static enum popup_kind popup_kind;
static gui_ui popup_ui;
static char search[64];
static gui_textbox_state searchbox;
static int perch_scroll;
static int perch_sel;
static int talon_scroll;
static int power_confirm = -1;

/* Cached panel button hit regions so mouse-down closes popups cleanly. */
static gui_rc perch_btn_rc;
static gui_rc talon_btn_rc;
static gui_rc quick_btn_rc;
static gui_rc hangar_btn_rc;

/* Hangar: parked apps, refreshed on open and after every park/resume. */
static struct k_hangar_info hangar_bays[K_HANGAR_BAYS];
static uint32_t hangar_thumbs[K_HANGAR_BAYS][K_HANGAR_THUMB_W * K_HANGAR_THUMB_H];
static int hangar_has_thumb[K_HANGAR_BAYS];
static int nhangar;
static int hangar_scroll;
static int hangar_confirm = -1;

/* ----------------------------------------------------------------- helpers */

static int win_ctl(uint32_t wid, int action)
{
    return gui_window_control(wid, action);
}

static int dock_h(const gui_settings *settings)
{
    return settings->compact ? DOCK_H_COMPACT : DOCK_H_FULL;
}

static int clicked(const gui_ui *ui, gui_rc r)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_LEFT) &&
           gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static int right_clicked(const gui_ui *ui, gui_rc r)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_RIGHT) &&
           gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static void notify(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void notify(const char *fmt, ...)
{
    va_list ap;

    if (toast[0])
        toast_needs_background = 1;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof(toast), fmt, ap);
    va_end(ap);
    toast_until = uptime_ms() + 4500;
}

static void refresh_app_availability(void)
{
    struct k_stat st;

    for (int i = 0; i < NAPPS; i++)
        apps[i].enabled = stat_(apps[i].path, &st) == 0 && !st.is_dir;
}

static int app_index(const char *path)
{
    for (int i = 0; i < NAPPS; i++)
        if (!strcmp(apps[i].path, path))
            return i;
    return -1;
}

static void launch_app(int index)
{
    char *argv[3];
    int pid;

    if (index < 0 || index >= NAPPS || !apps[index].enabled) {
        notify("%s is not installed", index >= 0 && index < NAPPS
                                      ? apps[index].name : "Application");
        return;
    }
    argv[0] = (char *)apps[index].path;
    argv[1] = (char *)apps[index].arg;
    argv[2] = 0;
    pid = spawn(apps[index].path, argv);
    if (pid < 0) {
        notify("Could not start %s", apps[index].name);
        return;
    }
    if (nchildren < MAX_CHILDREN)
        children[nchildren++] = pid;
    notify("Opening %s", apps[index].name);
}

static void reap_children(void)
{
    struct k_psinfo ps;

    for (int i = 0; i < nchildren; i++) {
        int found = 0, state = -1;

        for (int j = 0; psinfo(j, &ps) == 0; j++) {
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
        for (int j = i + 1; j < nchildren; j++)
            children[j - 1] = children[j];
        nchildren--;
        i--;
    }
}

static int lower_char(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int contains_case(const char *haystack, const char *needle)
{
    if (!needle || !*needle)
        return 1;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && lower_char(*h) == lower_char(*n)) {
            h++;
            n++;
        }
        if (!*n)
            return 1;
    }
    return 0;
}

/* Returns 0 on no match, >0 score on substring or subsequence fuzzy match. */
static int fuzzy_score(const char *haystack, const char *needle)
{
    int score = 0, run = 0, first = 1;
    const char *h, *n;

    if (!needle || !*needle)
        return 1;
    if (!haystack || !*haystack)
        return 0;
    if (contains_case(haystack, needle)) {
        score = 120;
        if (lower_char(haystack[0]) == lower_char(needle[0]))
            score += 40;
        return score;
    }

    h = haystack;
    n = needle;
    while (*n == ' ')
        n++;
    if (!*n)
        return 1;
    while (*h && *n) {
        if (*n == ' ') {
            n++;
            continue;
        }
        if (lower_char(*h) == lower_char(*n)) {
            run++;
            score += 10 + run * 4;
            if (first || h[-1] == ' ' || h[-1] == '-' || h[-1] == '/')
                score += 12;
            n++;
        } else {
            run = 0;
        }
        first = 0;
        h++;
    }
    while (*n == ' ')
        n++;
    return *n == '\0' ? score : 0;
}

static enum gui_icon title_icon(const char *title)
{
    if (strstr(title, "Sight"))
        return GUI_ICON_SEARCH;
    if (strstr(title, "Note"))
        return GUI_ICON_INFO;
    if (strstr(title, "HUD") || strstr(title, "KJS"))
        return GUI_ICON_TERMINAL;
    if (strstr(title, "Edit"))
        return GUI_ICON_PAINT;
    if (strstr(title, "Browser"))
        return GUI_ICON_BROWSER;
    if (strstr(title, "File"))
        return GUI_ICON_FILES;
    if (strstr(title, "Terminal"))
        return GUI_ICON_TERMINAL;
    if (strstr(title, "Paint"))
        return GUI_ICON_PAINT;
    if (strstr(title, "Clock"))
        return GUI_ICON_CLOCK;
    if (strstr(title, "Calculator"))
        return GUI_ICON_CALCULATOR;
    if (strstr(title, "Monitor"))
        return GUI_ICON_MONITOR;
    if (strstr(title, "Settings"))
        return GUI_ICON_SETTINGS;
    return GUI_ICON_INFO;
}

static int task_running_for_app(const struct app_entry *app)
{
    for (int i = 0; i < ntasks; i++) {
        if (contains_case(tasks[i].title, app->name))
            return 1;
        if (!strcmp(app->path, "/bin/gfiles") &&
            contains_case(tasks[i].title, "File"))
            return 1;
    }
    return 0;
}

static void clock_text(char *buf, unsigned long size, int include_date)
{
    struct gui_tm tm;
    unsigned long now = gui_time();

    if (now == 0 || (long)now < 0) {
        snprintf(buf, size, "--:--");
        return;
    }
    gui_gmtime(now, &tm);
    if (include_date)
        snprintf(buf, size, "%s %d %s  %02d:%02d",
                 gui_day_name(tm.wday), tm.day, gui_month_name(tm.mon),
                 tm.hour, tm.min);
    else
        snprintf(buf, size, "%02d:%02d", tm.hour, tm.min);
}

/* ----------------------------------------------------- state / pulse sync */

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_color_string(const char *s, int len, uint32_t *rgb_out)
{
    while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    if (len == 4 && s[0] == '#') {
        int r = hex_val(s[1]), g = hex_val(s[2]), b = hex_val(s[3]);
        if (r < 0 || g < 0 || b < 0)
            return -1;
        *rgb_out = GUI_RGB(r * 17, g * 17, b * 17);
        return 0;
    }
    if ((len == 7 || len == 9) && s[0] == '#') {
        uint32_t v = 0;
        for (int i = 1; i < 7; i++) {
            int d = hex_val(s[i]);
            if (d < 0)
                return -1;
            v = (v << 4) | (uint32_t)d;
        }
        *rgb_out = v & 0xFFFFFFU;
        return 0;
    }
    if (len == 8 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t v = 0;
        for (int i = 2; i < 8; i++) {
            int d = hex_val(s[i]);
            if (d < 0)
                return -1;
            v = (v << 4) | (uint32_t)d;
        }
        *rgb_out = v & 0xFFFFFFU;
        return 0;
    }
    return -1;
}

static void refresh_tasks(void)
{
    int n = 0;

    while (n < MAX_TASKS && gui_window_list(n, &tasks[n]) == 0)
        n++;
    ntasks = n;
    if (ntasks > 0) {
        active_ws = (int)((tasks[0].state >> K_WIN_STATE_ACTIVE_WS_SHIFT) &
                          K_WIN_STATE_WS_MASK);
    }
}

static void refresh_hangar(void)
{
    int n = 0;

    while (n < K_HANGAR_BAYS && hangar_list(n, &hangar_bays[n]) == 0) {
        hangar_has_thumb[n] = hangar_thumb((int)hangar_bays[n].bay,
                                           hangar_thumbs[n]) == 0;
        n++;
    }
    nhangar = n;
}

static int task_for_app(const struct app_entry *app)
{
    for (int i = 0; i < ntasks; i++) {
        if (contains_case(tasks[i].title, app->name))
            return i;
        if (!strcmp(app->path, "/bin/gfiles") &&
            contains_case(tasks[i].title, "File"))
            return i;
    }
    return -1;
}

/* Park the process behind one window. The kernel blocks until the image
 * is on disk, or explains in one sentence why the app cannot be parked. */
static void park_task(const struct k_winsummary *t)
{
    char reason[K_HANGAR_REASON_MAX];
    char title[64];
    int bay;

    memcpy(title, t->title, sizeof(title));
    title[sizeof(title) - 1] = '\0';
    reason[0] = '\0';
    bay = hangar_park(t->pid, reason);
    if (bay < 0)
        notify("Hangar: %s", reason[0] ? reason : "could not park the app");
    else
        notify("%s parked in Hangar bay %02d", title, bay);
    refresh_tasks();
    refresh_hangar();
}

static void park_window(uint32_t wid)
{
    refresh_tasks();
    for (int i = 0; i < ntasks; i++) {
        if (tasks[i].wid == wid) {
            park_task(&tasks[i]);
            return;
        }
    }
    notify("Hangar: that window is gone");
}

static int resume_bay(int idx)
{
    char reason[K_HANGAR_REASON_MAX];
    struct k_hangar_info info = hangar_bays[idx];
    int pid;

    reason[0] = '\0';
    pid = hangar_resume((int)info.bay, reason);
    if (pid < 0) {
        notify("Hangar: %s", reason[0] ? reason : "could not resume");
        refresh_hangar();
        return -1;
    }
    if (nchildren < MAX_CHILDREN)
        children[nchildren++] = pid;
    notify("%s cleared for takeoff from bay %02u",
           info.nwins ? info.title : info.name, info.bay);
    refresh_hangar();
    return 0;
}

static void refresh_net(void)
{
    memset(&net_cache, 0, sizeof(net_cache));
    net_online = netinfo(&net_cache) == 0 && net_cache.up;
}

static void refresh_talon(void)
{
    struct k_parcel_info info;
    int i;

    ntalon = 0;
    for (i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
        char raw[96];
        long got;
        int o = 0;

        if (parcel_list(i, &info) != 0)
            break;
        talon_slots[ntalon].info = info;
        talon_slots[ntalon].info.mime[sizeof(info.mime) - 1] = '\0';
        talon_slots[ntalon].info.label[sizeof(info.label) - 1] = '\0';
        talon_slots[ntalon].preview[0] = '\0';
        talon_slots[ntalon].has_color = 0;
        talon_slots[ntalon].color_rgb = 0;

        got = parcel_get(info.id, 0, raw, sizeof(raw) - 1);
        if (got > 0) {
            raw[got] = '\0';
            if (info.type == K_PARCEL_COLOR &&
                parse_color_string(raw, (int)got,
                                   &talon_slots[ntalon].color_rgb) == 0) {
                talon_slots[ntalon].has_color = 1;
            }
            for (long k = 0; k < got && o < (int)sizeof(talon_slots[0].preview) - 1; k++) {
                unsigned char ch = (unsigned char)raw[k];
                if (ch == '\n' || ch == '\r' || ch == '\t')
                    talon_slots[ntalon].preview[o++] = ' ';
                else if (ch >= 0x20 && ch <= 0x7E)
                    talon_slots[ntalon].preview[o++] = (char)ch;
            }
            talon_slots[ntalon].preview[o] = '\0';
        }
        ntalon++;
    }
}

static int memory_percent(uint64_t *used_mib, uint64_t *total_mib)
{
    uint64_t total = 0, free = 0, used;

    if (meminfo(&total, &free) != 0 || total == 0) {
        *used_mib = *total_mib = 0;
        return 0;
    }
    used = total - free;
    *used_mib = used / 1024;
    *total_mib = total / 1024;
    return (int)((used * 100) / total);
}

static void sample_telemetry(void)
{
    struct k_cpuinfo cpu;
    struct k_psinfo ps;
    uint64_t used_mib = 0, total_mib = 0;
    int ram_pct = memory_percent(&used_mib, &total_mib);
    int online = 1, active_tasks = 0, total_tasks = 0, raw_cpu, cpu_pct;

    memset(&cpu, 0, sizeof(cpu));
    if (cpuinfo(&cpu) == 0 && cpu.online > 0)
        online = (int)cpu.online;

    for (int i = 0; psinfo(i, &ps) == 0; i++) {
        if (strncmp(ps.name, "idle/", 5) == 0)
            continue;
        total_tasks++;
        if (ps.state == K_STATE_RUNNING)
            active_tasks += 2;
        else if (ps.state == K_STATE_RUNNABLE)
            active_tasks += 1;
    }

    raw_cpu = 4 + (active_tasks * 18) / online + total_tasks / 3 +
              (int)(pulse_burst * 5);
    pulse_burst = 0;
    if (raw_cpu > 98)
        raw_cpu = 98;
    if (raw_cpu < 3)
        raw_cpu = 3;

    cpu_pct = last_cpu_pct > 0 ? (last_cpu_pct * 2 + raw_cpu) / 3 : raw_cpu;
    last_cpu_pct = cpu_pct;
    last_ram_pct = ram_pct;

    for (int i = 0; i < SPARK_SAMPLES - 1; i++) {
        cpu_hist[i] = cpu_hist[i + 1];
        ram_hist[i] = ram_hist[i + 1];
    }
    cpu_hist[SPARK_SAMPLES - 1] = cpu_pct;
    ram_hist[SPARK_SAMPLES - 1] = ram_pct;
}

static void init_pulses(void)
{
    struct k_pulse_query q;

    memset(&q, 0, sizeof(q));
    q.seq = 0;
    q.topic_mask = K_TOPIC_WIN | K_TOPIC_PROC | K_TOPIC_PARCEL |
                   K_TOPIC_THEME | K_TOPIC_NET;
    q.max_out = 0;
    q.timeout_ms = 0;
    if (pulse_poll(&q, 0) >= 0)
        pulse_seq = q.seq;
}

static void poll_pulses(gui_settings *settings, int *wallpaper_dirty,
                        int *shell_dirty, int *popup_dirty)
{
    struct k_pulse_query q;
    struct k_pulse pulses[16];
    int got_win = 0, got_proc = 0, got_parcel = 0, got_theme = 0, got_net = 0;

    for (;;) {
        int n;

        memset(&q, 0, sizeof(q));
        q.seq = pulse_seq;
        q.topic_mask = K_TOPIC_WIN | K_TOPIC_PROC | K_TOPIC_PARCEL |
                       K_TOPIC_THEME | K_TOPIC_NET;
        q.max_out = 16;
        q.timeout_ms = 0;
        n = pulse_poll(&q, pulses);
        if (n <= 0) {
            if (n == 0)
                pulse_seq = q.seq;
            break;
        }
        pulse_seq = q.seq;
        pulse_burst += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            if (pulses[i].topic & K_TOPIC_WIN) {
                got_win = 1;
                if (pulses[i].kind == K_PULSE_WIN_WS)
                    active_ws = (int)(pulses[i].arg0 & K_WIN_STATE_WS_MASK);
            }
            if (pulses[i].topic & K_TOPIC_PROC)
                got_proc = 1;
            if (pulses[i].topic & K_TOPIC_PARCEL)
                got_parcel = 1;
            if (pulses[i].topic & K_TOPIC_THEME)
                got_theme = 1;
            if (pulses[i].topic & K_TOPIC_NET)
                got_net = 1;
        }
        if (n < 16)
            break;
    }

    if (got_proc) {
        reap_children();
        *shell_dirty = 1;
    }
    if (got_win) {
        refresh_tasks();
        *shell_dirty = 1;
        if (popup && popup_kind == POP_LAUNCHER)
            *popup_dirty = 1;
    }
    if (got_parcel) {
        refresh_talon();
        *shell_dirty = 1;
        if (popup && popup_kind == POP_TALON)
            *popup_dirty = 1;
    }
    if (got_net) {
        refresh_net();
        *shell_dirty = 1;
        if (popup && popup_kind == POP_QUICK)
            *popup_dirty = 1;
    }
    if (got_theme) {
        gui_settings_reload();
        gui_settings_get(settings);
        *wallpaper_dirty = 1;
        *shell_dirty = 1;
        if (popup)
            *popup_dirty = 1;
    }
}

/* ----------------------------------------------------- Perch evaluator */

struct expr_parser {
    const char *p;
    int err;
    int depth;
};

static void ep_skip_ws(struct expr_parser *ep)
{
    while (*ep->p == ' ' || *ep->p == '\t')
        ep->p++;
}

static long ep_or(struct expr_parser *ep);

static long ep_primary(struct expr_parser *ep)
{
    unsigned long uv = 0;
    int digits = 0;

    ep_skip_ws(ep);
    if (*ep->p == '(') {
        long v;
        ep->p++;
        v = ep_or(ep);
        ep_skip_ws(ep);
        if (*ep->p != ')') {
            ep->err = 1;
            return 0;
        }
        ep->p++;
        return v;
    }
    if (ep->p[0] == '0' && (ep->p[1] == 'x' || ep->p[1] == 'X')) {
        ep->p += 2;
        while (hex_val(*ep->p) >= 0) {
            unsigned d = (unsigned)hex_val(*ep->p++);
            if (uv > ((unsigned long)CALC_LONG_MAX - d) / 16UL) {
                ep->err = 2;
                return 0;
            }
            uv = uv * 16UL + d;
            digits = 1;
        }
        if (!digits) {
            ep->err = 1;
            return 0;
        }
        return (long)uv;
    }
    if (ep->p[0] == '0' && (ep->p[1] == 'b' || ep->p[1] == 'B')) {
        ep->p += 2;
        while (*ep->p == '0' || *ep->p == '1') {
            unsigned d = (unsigned)(*ep->p++ - '0');
            if (uv > ((unsigned long)CALC_LONG_MAX - d) / 2UL) {
                ep->err = 2;
                return 0;
            }
            uv = uv * 2UL + d;
            digits = 1;
        }
        if (!digits) {
            ep->err = 1;
            return 0;
        }
        return (long)uv;
    }
    if (*ep->p < '0' || *ep->p > '9') {
        ep->err = 1;
        return 0;
    }
    while (*ep->p >= '0' && *ep->p <= '9') {
        unsigned d = (unsigned)(*ep->p++ - '0');
        if (uv > ((unsigned long)CALC_LONG_MAX - d) / 10UL) {
            ep->err = 2;
            return 0;
        }
        uv = uv * 10UL + d;
    }
    return (long)uv;
}

static long ep_unary(struct expr_parser *ep)
{
    long v;

    if (ep->depth >= 32) {
        ep->err = 1;
        return 0;
    }
    ep_skip_ws(ep);
    if (*ep->p == '-' || *ep->p == '+' || *ep->p == '~') {
        char op = *ep->p++;
        ep->depth++;
        v = ep_unary(ep);
        ep->depth--;
        if (ep->err)
            return 0;
        if (op == '-') {
            if (v == CALC_LONG_MIN) {
                ep->err = 2;
                return 0;
            }
            return -v;
        }
        if (op == '~')
            return ~v;
        return v;
    }
    ep->depth++;
    v = ep_primary(ep);
    ep->depth--;
    return v;
}

static long ep_mul(struct expr_parser *ep)
{
    long v = ep_unary(ep);

    for (;;) {
        char op;
        long r;
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        op = *ep->p;
        if (op != '*' && op != '/' && op != '%')
            return v;
        ep->p++;
        r = ep_unary(ep);
        if (ep->err)
            return 0;
        if (op == '*') {
            if (__builtin_mul_overflow(v, r, &v)) {
                ep->err = 2;
                return 0;
            }
        } else {
            if (r == 0) {
                ep->err = 3;
                return 0;
            }
            if (v == CALC_LONG_MIN && r == -1) {
                ep->err = 2;
                return 0;
            }
            v = op == '/' ? (v / r) : (v % r);
        }
    }
}

static long ep_add(struct expr_parser *ep)
{
    long v = ep_mul(ep);

    for (;;) {
        char op;
        long r;
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        op = *ep->p;
        if (op != '+' && op != '-')
            return v;
        ep->p++;
        r = ep_mul(ep);
        if (ep->err)
            return 0;
        if (op == '+') {
            if (__builtin_add_overflow(v, r, &v)) {
                ep->err = 2;
                return 0;
            }
        } else {
            if (__builtin_sub_overflow(v, r, &v)) {
                ep->err = 2;
                return 0;
            }
        }
    }
}

static long ep_shift(struct expr_parser *ep)
{
    long v = ep_add(ep);

    for (;;) {
        int dir = 0;
        long r;
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        if (ep->p[0] == '<' && ep->p[1] == '<')
            dir = 1;
        else if (ep->p[0] == '>' && ep->p[1] == '>')
            dir = 2;
        else
            return v;
        ep->p += 2;
        r = ep_add(ep);
        if (ep->err)
            return 0;
        if (r < 0 || r >= 63) {
            ep->err = 2;
            return 0;
        }
        v = (dir == 1) ? (long)((unsigned long)v << r) : (v >> r);
    }
}

static long ep_and(struct expr_parser *ep)
{
    long v = ep_shift(ep);

    for (;;) {
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        if (*ep->p != '&')
            return v;
        ep->p++;
        v &= ep_shift(ep);
    }
}

static long ep_xor(struct expr_parser *ep)
{
    long v = ep_and(ep);

    for (;;) {
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        if (*ep->p != '^')
            return v;
        ep->p++;
        v ^= ep_and(ep);
    }
}

static long ep_or(struct expr_parser *ep)
{
    long v = ep_xor(ep);

    for (;;) {
        if (ep->err)
            return 0;
        ep_skip_ws(ep);
        if (*ep->p != '|')
            return v;
        ep->p++;
        v |= ep_xor(ep);
    }
}

static void eval_perch_query(const char *query, struct perch_eval *out)
{
    const char *q = query;
    struct expr_parser ep;
    uint32_t rgb = 0;
    long val;
    int explicit_eq = 0;

    memset(out, 0, sizeof(*out));
    while (*q == ' ' || *q == '\t')
        q++;
    if (!*q)
        return;

    if (strncmp(q, "@krec", 5) == 0 &&
        (q[5] == '\0' || q[5] == ' ' || q[5] == '\t')) {
        out->valid = 1;
        out->parcel_type = K_PARCEL_TEXT;
        snprintf(out->title, sizeof(out->title),
                 "@krec Typed Record Protocol");
        snprintf(out->subtitle, sizeof(out->subtitle),
                 "Header: @krec col:type ...  |  str int u64 hex bytes ip bool");
        snprintf(out->copy_text, sizeof(out->copy_text),
                 "@krec name:str value:int");
        snprintf(out->copy_label, sizeof(out->copy_label), "@krec schema");
        return;
    }

    if (q[0] == '#' && parse_color_string(q, (int)strlen(q), &rgb) == 0) {
        out->valid = 1;
        out->has_color = 1;
        out->color_rgb = rgb;
        out->parcel_type = K_PARCEL_COLOR;
        snprintf(out->title, sizeof(out->title), "#%06X", rgb);
        snprintf(out->subtitle, sizeof(out->subtitle),
                 "RGB(%u, %u, %u)  -  Enter copies color to Talon",
                 GUI_RED(rgb), GUI_GREEN(rgb), GUI_BLUE(rgb));
        snprintf(out->copy_text, sizeof(out->copy_text), "#%06X", rgb);
        snprintf(out->copy_label, sizeof(out->copy_label), "Color #%06X", rgb);
        return;
    }

    if (*q == '=') {
        explicit_eq = 1;
        q++;
        while (*q == ' ' || *q == '\t')
            q++;
        if (!*q) {
            out->is_error = 1;
            snprintf(out->title, sizeof(out->title), "Evaluator");
            snprintf(out->subtitle, sizeof(out->subtitle),
                     "Type arithmetic, 0x hex, 0b bin, #RRGGBB, or @krec");
            return;
        }
    } else {
        int looks_num = (q[0] >= '0' && q[0] <= '9') ||
                        q[0] == '(' || q[0] == '-' || q[0] == '~';
        if (!looks_num)
            return;
    }

    ep.p = q;
    ep.err = 0;
    ep.depth = 0;
    val = ep_or(&ep);
    ep_skip_ws(&ep);
    if (ep.err || *ep.p != '\0') {
        if (explicit_eq) {
            const char *emsg = ep.err == 3 ? "Division by zero"
                             : ep.err == 2 ? "64-bit integer overflow"
                                           : "Expected + - * / % << >> & | ^ or ()";
            out->is_error = 1;
            snprintf(out->title, sizeof(out->title), "Expression error");
            snprintf(out->subtitle, sizeof(out->subtitle), "%s", emsg);
        }
        return;
    }

    out->valid = 1;
    out->parcel_type = K_PARCEL_NUMBER;
    snprintf(out->title, sizeof(out->title), "%ld", val);
    snprintf(out->subtitle, sizeof(out->subtitle),
             "dec %ld   hex 0x%lx   Enter -> Talon",
             val, (unsigned long)val);
    snprintf(out->copy_text, sizeof(out->copy_text), "%ld", val);
    snprintf(out->copy_label, sizeof(out->copy_label), "%s", q);
}

/* ---------------------------------------------------------------- wallpaper */

static void text_scaled(gui_window *win, int x, int y, const char *s,
                        int scale, uint32_t color)
{
    for (; *s; s++) {
        const uint8_t *g = gui_glyph((unsigned char)*s);

        for (int row = 0; row < GUI_FONT_H; row++)
            for (int col = 0; col < GUI_FONT_W; col++)
                if (g[row] & (0x80 >> col))
                    gui_rect(win, x + col * scale, y + row * scale,
                             scale, scale, color);
        x += GUI_FONT_W * scale;
    }
}

static gui_rc shortcut_rc(int index)
{
    return gui_mkrc(30, TOP_H + 34 + index * 86, 188, 70);
}

static void paint_shortcut(gui_window *win, int index, const char *path,
                           const char *sub)
{
    int app = app_index(path);
    gui_rc r = shortcut_rc(index);
    gui_rc icon = gui_mkrc(r.x + 12, r.y + 11, 48, 48);

    if (app < 0)
        return;
    gui_shadow(win, r, 10, gui_mix(GUI_BORDER, GUI_DESK_BOTTOM, 80));
    gui_round_rect(win, r, 10, gui_mix(GUI_SURFACE, GUI_DESK_TOP, 70));
    gui_round_frame(win, r, 10, gui_mix(GUI_EDGE, GUI_DESK_TOP, 100));
    gui_round_rect(win, icon, 12,
                   apps[app].enabled ? apps[app].color : GUI_TEXT_DIM);
    gui_icon(win, icon.x + 10, icon.y + 10, 28, apps[app].icon, GUI_WHITE);
    gui_text(win, r.x + 72, r.y + 15, apps[app].name,
             apps[app].enabled ? GUI_TEXT : GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_text(win, r.x + 72, r.y + 37, sub, GUI_TEXT_DIM, GUI_TRANSPARENT);
}

static void paint_wallpaper(gui_window *win, const gui_settings *settings)
{
    int h = win->h - dock_h(settings);
    uint32_t top = GUI_DESK_TOP, bottom = GUI_DESK_BOTTOM;

    if (settings->wallpaper == 3) {
        top = gui_mix(GUI_DESK_TOP, 0x4B244AU, 104);
        bottom = gui_mix(GUI_DESK_BOTTOM, 0x19143BU, 80);
    }
    gui_vgradient(win, gui_mkrc(0, 0, win->w, h), top, bottom);

    switch (settings->wallpaper) {
    case 0: /* aurora */
        gui_disc(win, win->w - 110, 120, 340,
                 gui_mix(top, GUI_ACCENT, 30));
        gui_disc(win, win->w - 10, h - 60, 270,
                 gui_mix(bottom, 0x8B5CF6U, 28));
        gui_disc(win, win->w / 2 + 80, -120, 260,
                 gui_mix(top, 0x2DD4BFU, 18));
        break;
    case 1: /* contour */
        for (int i = -6; i < 16; i++)
            gui_line_w(win, 0, 120 + i * 48, win->w,
                       -80 + i * 48, gui_mix(top, GUI_ACCENT, 24), 2);
        break;
    case 2: /* quiet */
        gui_round_frame(win, gui_mkrc(win->w / 2 - 250, h / 2 - 150,
                                      500, 300), 36,
                        gui_mix(top, GUI_TEXT, 18));
        break;
    default: /* sunset */
        gui_disc(win, win->w - 180, h / 2, 250,
                 gui_mix(top, 0xFB7185U, 46));
        gui_disc(win, win->w - 90, h / 2 + 100, 190,
                 gui_mix(bottom, 0xFBBF24U, 28));
        break;
    }

    text_scaled(win, win->w - 420, h - 164, "Kestrel", 3,
                gui_mix(bottom, GUI_TEXT, settings->light ? 70 : 40));
    gui_draw_text_aa(win, win->w - 418, h - 106,
                     "Avionics Desktop  -  Super: Perch  Alt+1..4: Workspaces",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_TEXT_DIM, GUI_TRANSPARENT);

    paint_shortcut(win, 0, "/bin/gfiles", "Home folder");
    paint_shortcut(win, 1, "/bin/browser", "Start browsing");
    paint_shortcut(win, 2, "/bin/terminal", "Shell & tools");
}

/* ----------------------------------------------------- Top Telemetry Panel */

static void close_popup(void);
static void open_popup(enum popup_kind kind, int sw, int sh,
                       const gui_settings *settings);

static void task_title(const char *src, char out[20], int max_chars)
{
    int i;

    if (max_chars < 1)
        max_chars = 1;
    if (max_chars > 18)
        max_chars = 18;
    for (i = 0; i < max_chars && src[i]; i++)
        out[i] = src[i];
    if (src[i] && i >= 3) {
        out[i - 3] = '.';
        out[i - 2] = '.';
        out[i - 1] = '.';
    }
    out[i] = '\0';
}

static void paint_topbar(gui_window *win, gui_ui *ui)
{
    char clock_s[32], talon_lbl[24], cpu_s[16], ram_s[16];
    int compact_bar = win->w < 920;
    int x = 8;
    int clock_w, net_w, spark_w, talon_w, right_x;
    gui_rc clock_rc, net_rc, spark_rc;
    int ws_tasks[MAX_TASKS], n_ws_tasks = 0;

    gui_rect(win, 0, 0, win->w, TOP_H, gui_mix(GUI_PANEL, GUI_BLACK, 26));
    gui_rect(win, 0, TOP_H - 1, win->w, 1, GUI_EDGE);

    /* 1. [Perch] Apps button + Super badge */
    perch_btn_rc = gui_mkrc(x, 4, compact_bar ? 84 : 104, TOP_H - 8);
    {
        int hov = gui_hit(perch_btn_rc, ui->mx, ui->my);
        int act = popup_kind == POP_LAUNCHER;
        uint32_t bg = act ? GUI_ACCENT_DARK
                    : hov ? GUI_PANEL_HI
                          : gui_mix(GUI_SURFACE, GUI_PANEL, 128);
        uint32_t bd = act ? GUI_ACCENT : hov ? GUI_EDGE : GUI_BORDER;

        gui_fill_round_rect(win, perch_btn_rc, 7, bg);
        gui_stroke_round_rect(win, perch_btn_rc, 7, 1, bd);
        gui_icon(win, perch_btn_rc.x + 7, perch_btn_rc.y + 5, 16,
                 GUI_ICON_APPS, act ? GUI_WHITE : GUI_ACCENT);
        gui_draw_text_aa(win, perch_btn_rc.x + 26, perch_btn_rc.y + 6,
                         "Perch", FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         act ? GUI_WHITE : GUI_TEXT, GUI_TRANSPARENT);
        if (!compact_bar) {
            gui_rc sup = gui_mkrc(perch_btn_rc.x + perch_btn_rc.w - 34,
                                  perch_btn_rc.y + 4, 28, 18);
            gui_draw_pill(win, sup,
                          act ? gui_mix(GUI_ACCENT, GUI_BLACK, 80) : GUI_SUNKEN,
                          (long)(act ? GUI_WHITE : GUI_EDGE));
            gui_draw_text_aa(win, sup.x + 5, sup.y + 3, "Sup",
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             act ? GUI_WHITE : GUI_TEXT_DIM, GUI_TRANSPARENT);
        }
        if (clicked(ui, perch_btn_rc)) {
            if (popup_kind == POP_LAUNCHER)
                close_popup();
            else
                open_popup(POP_LAUNCHER, win->w, win->h, 0);
        }
    }
    x += perch_btn_rc.w + 8;

    /* 2. Wind-Hover Workspaces 1 2 3 4 pill strip */
    {
        int ws_w = compact_bar ? 28 : 32;
        int ws_strip_w = K_WM_MAX_WORKSPACES * ws_w +
                         (K_WM_MAX_WORKSPACES - 1) * 3 + 6;
        gui_rc strip = gui_mkrc(x, 3, ws_strip_w, TOP_H - 6);

        gui_fill_round_rect(win, strip, 8, GUI_SUNKEN);
        gui_stroke_round_rect(win, strip, 8, 1, GUI_BORDER);

        for (int ws = 0; ws < K_WM_MAX_WORKSPACES; ws++) {
            gui_rc wr = gui_mkrc(x + 3 + ws * (ws_w + 3), 5, ws_w, TOP_H - 10);
            int is_act = (ws == active_ws);
            int hov = gui_hit(wr, ui->mx, ui->my);
            int win_cnt = 0;
            char num[2];

            for (int k = 0; k < ntasks; k++) {
                int w_ws = (int)((tasks[k].state >> K_WIN_STATE_WS_SHIFT) &
                                 K_WIN_STATE_WS_MASK);
                int pinned = (tasks[k].state & K_WIN_STATE_PINNED) != 0;
                if (w_ws == ws || pinned)
                    win_cnt++;
            }

            if (is_act) {
                gui_fill_round_rect(win, wr, 6, GUI_ACCENT_DARK);
                gui_stroke_round_rect(win, wr, 6, 1, GUI_ACCENT);
            } else if (hov) {
                gui_fill_round_rect(win, wr, 6, GUI_PANEL_HI);
                gui_stroke_round_rect(win, wr, 6, 1, GUI_EDGE);
            }

            num[0] = (char)('1' + ws);
            num[1] = '\0';
            gui_draw_text_aa(win, wr.x + (wr.w - 6) / 2, wr.y + 3, num,
                             FONT_SMALL, is_act ? FONT_BOLD : FONT_REGULAR,
                             FONT_ROMAN,
                             is_act ? GUI_WHITE
                                    : (win_cnt > 0 ? GUI_TEXT : GUI_TEXT_DIM),
                             GUI_TRANSPARENT);

            if (win_cnt > 0) {
                int dots = win_cnt > 4 ? 4 : win_cnt;
                int total_dw = dots * 4 + (dots - 1) * 2;
                int dx = wr.x + (wr.w - total_dw) / 2;
                uint32_t dc = is_act ? GUI_WHITE : GUI_ACCENT;

                for (int d = 0; d < dots; d++)
                    gui_rect(win, dx + d * 6, wr.y + wr.h - 4, 3, 2, dc);
            }

            if (clicked(ui, wr)) {
                if (win_ctl((uint32_t)ws, K_WIN_CTL_SWITCH_WS) == 0) {
                    active_ws = ws;
                    refresh_tasks();
                }
            }
        }
        x += ws_strip_w + 8;
    }

    /* 3. Right telemetry cluster */
    clock_text(clock_s, sizeof(clock_s), win->w >= 1160);
    clock_w = gui_text_w(clock_s) + 18;
    net_w = compact_bar ? 46 : 54;
    spark_w = compact_bar ? 104 : 124;
    talon_w = compact_bar ? 96 : 134;

    right_x = win->w - 8 - clock_w;
    clock_rc = gui_mkrc(right_x, 4, clock_w, TOP_H - 8);

    right_x -= (4 + net_w);
    net_rc = gui_mkrc(right_x, 4, net_w, TOP_H - 8);

    right_x -= (4 + spark_w);
    spark_rc = gui_mkrc(right_x, 4, spark_w, TOP_H - 8);

    right_x -= (4 + talon_w);
    talon_btn_rc = gui_mkrc(right_x, 4, talon_w, TOP_H - 8);

    /* Clock pill */
    {
        int hov = gui_hit(clock_rc, ui->mx, ui->my);
        gui_fill_round_rect(win, clock_rc, 7,
                            hov ? GUI_PANEL_HI : GUI_SUNKEN);
        gui_stroke_round_rect(win, clock_rc, 7, 1,
                              hov ? GUI_EDGE : GUI_BORDER);
        gui_text(win, clock_rc.x + 9, clock_rc.y + 5, clock_s, GUI_TEXT,
                 GUI_TRANSPARENT);
        if (clicked(ui, clock_rc)) {
            if (popup_kind == POP_QUICK)
                close_popup();
            else
                open_popup(POP_QUICK, win->w, win->h, 0);
        }
    }

    /* Network badge */
    {
        int hov = gui_hit(net_rc, ui->mx, ui->my);
        uint32_t nc = net_online ? GUI_OK : GUI_TEXT_DIM;
        gui_fill_round_rect(win, net_rc, 7,
                            hov ? GUI_PANEL_HI : GUI_SUNKEN);
        gui_stroke_round_rect(win, net_rc, 7, 1,
                              net_online ? gui_mix(GUI_OK, GUI_BORDER, 140)
                                         : GUI_BORDER);
        gui_icon(win, net_rc.x + 6, net_rc.y + 5, 16, GUI_ICON_NETWORK, nc);
        gui_draw_text_aa(win, net_rc.x + 24, net_rc.y + 7,
                         net_online ? "NET" : "OFF",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         net_online ? GUI_OK : GUI_TEXT_DIM,
                         GUI_TRANSPARENT);
        if (clicked(ui, net_rc)) {
            if (popup_kind == POP_QUICK)
                close_popup();
            else
                open_popup(POP_QUICK, win->w, win->h, 0);
        }
    }

    /* Live CPU & RAM mini sparklines */
    {
        int hov = gui_hit(spark_rc, ui->mx, ui->my);
        int half = (spark_rc.w - 10) / 2;
        gui_rc cpu_sp = gui_mkrc(spark_rc.x + 4, spark_rc.y + 10,
                                 half - 2, spark_rc.h - 12);
        gui_rc ram_sp = gui_mkrc(spark_rc.x + 6 + half, spark_rc.y + 10,
                                 half - 2, spark_rc.h - 12);

        gui_fill_round_rect(win, spark_rc, 7,
                            hov ? GUI_PANEL_HI : GUI_SUNKEN);
        gui_stroke_round_rect(win, spark_rc, 7, 1,
                              hov ? GUI_EDGE : GUI_BORDER);
        snprintf(cpu_s, sizeof(cpu_s), "C%d%%", last_cpu_pct);
        snprintf(ram_s, sizeof(ram_s), "M%d%%", last_ram_pct);
        gui_draw_text_aa(win, spark_rc.x + 4, spark_rc.y + 1, cpu_s,
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_OK, GUI_TRANSPARENT);
        gui_draw_text_aa(win, spark_rc.x + 6 + half, spark_rc.y + 1, ram_s,
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_ACCENT, GUI_TRANSPARENT);
        gui_draw_sparkline(win, cpu_sp, cpu_hist, SPARK_SAMPLES, 100,
                           GUI_OK, (long)GUI_OK);
        gui_draw_sparkline(win, ram_sp, ram_hist, SPARK_SAMPLES, 100,
                           GUI_ACCENT, (long)GUI_ACCENT);
        if (clicked(ui, spark_rc))
            launch_app(app_index("/bin/sysmon"));
    }

    /* Talon Shelf button */
    {
        int hov = gui_hit(talon_btn_rc, ui->mx, ui->my);
        int act = popup_kind == POP_TALON;
        char cnt_s[8];
        gui_rc cnt_rc = gui_mkrc(talon_btn_rc.x + 5, talon_btn_rc.y + 4,
                                 18, 18);

        gui_fill_round_rect(win, talon_btn_rc, 7,
                            act ? GUI_ACCENT_DARK
                                : (hov ? GUI_PANEL_HI : GUI_SUNKEN));
        gui_stroke_round_rect(win, talon_btn_rc, 7, 1,
                              act ? GUI_ACCENT : (hov ? GUI_EDGE : GUI_BORDER));

        snprintf(cnt_s, sizeof(cnt_s), "%d", ntalon);
        gui_draw_pill(win, cnt_rc,
                      ntalon > 0 ? (act ? GUI_ACCENT : GUI_ACCENT_DARK)
                                 : GUI_PANEL,
                      (long)(ntalon > 0 ? GUI_ACCENT : GUI_BORDER));
        gui_draw_text_aa(win, cnt_rc.x + 6, cnt_rc.y + 3, cnt_s,
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         ntalon > 0 ? GUI_WHITE : GUI_TEXT_DIM,
                         GUI_TRANSPARENT);

        if (ntalon > 0 && talon_slots[0].has_color) {
            gui_fill_round_rect(win, gui_mkrc(talon_btn_rc.x + 27,
                                              talon_btn_rc.y + 6, 14, 14),
                                3, talon_slots[0].color_rgb);
            gui_stroke_round_rect(win, gui_mkrc(talon_btn_rc.x + 27,
                                                talon_btn_rc.y + 6, 14, 14),
                                  3, 1, GUI_EDGE);
        }

        if (ntalon == 0) {
            snprintf(talon_lbl, sizeof(talon_lbl), "Talon");
        } else if (talon_slots[0].info.label[0]) {
            snprintf(talon_lbl, sizeof(talon_lbl), "%s",
                     talon_slots[0].info.label);
        } else if (talon_slots[0].preview[0]) {
            snprintf(talon_lbl, sizeof(talon_lbl), "%s",
                     talon_slots[0].preview);
        } else {
            snprintf(talon_lbl, sizeof(talon_lbl), "Parcel #%u",
                     talon_slots[0].info.id);
        }
        {
            int tx = talon_btn_rc.x +
                     ((ntalon > 0 && talon_slots[0].has_color) ? 45 : 27);
            int max_tw = talon_btn_rc.x + talon_btn_rc.w - tx - 5;
            if (max_tw > 12) {
                gui_draw_text_aa_run(win, tx, talon_btn_rc.y + 7, talon_lbl,
                                     -1, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                     act ? GUI_WHITE
                                         : (ntalon > 0 ? GUI_TEXT : GUI_TEXT_DIM),
                                     GUI_TRANSPARENT, max_tw, 1);
            }
        }
        if (clicked(ui, talon_btn_rc)) {
            if (popup_kind == POP_TALON)
                close_popup();
            else
                open_popup(POP_TALON, win->w, win->h, 0);
        }
    }

    /* 4. Middle: Active workspace task pills */
    for (int i = 0; i < ntasks; i++) {
        int w_ws = (int)((tasks[i].state >> K_WIN_STATE_WS_SHIFT) &
                         K_WIN_STATE_WS_MASK);
        int pinned = (tasks[i].state & K_WIN_STATE_PINNED) != 0;
        if (w_ws == active_ws || pinned)
            ws_tasks[n_ws_tasks++] = i;
    }

    {
        int mid_x = x;
        int mid_end = right_x - 8;
        int avail = mid_end - mid_x;

        if (n_ws_tasks == 0 && avail > 120) {
            char hint[48];
            snprintf(hint, sizeof(hint), "WS %d  -  Clear airspace",
                     active_ws + 1);
            gui_draw_text_aa_run(win, mid_x + 4, 10, hint, -1,
                                 FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                 GUI_TEXT_DIM, GUI_TRANSPARENT,
                                 avail - 8, 1);
        } else if (n_ws_tasks > 0 && avail >= 48) {
            int pill_w = avail / n_ws_tasks;
            if (pill_w > 174)
                pill_w = 174;
            if (pill_w < 54)
                pill_w = 54;

            for (int j = 0; j < n_ws_tasks && mid_x + pill_w <= mid_end; j++) {
                const struct k_winsummary *t = &tasks[ws_tasks[j]];
                int focused = (t->state & K_WIN_STATE_FOCUSED) != 0;
                int minimized = (t->state & K_WIN_STATE_MINIMIZED) != 0;
                int pinned = (t->state & K_WIN_STATE_PINNED) != 0;
                gui_rc pr = gui_mkrc(mid_x, 4, pill_w - 4, TOP_H - 8);
                int show_pin = pr.w >= 96;
                int show_close = pr.w >= 76;
                gui_rc close_rc = show_close
                    ? gui_mkrc(pr.x + pr.w - 18, pr.y + 5, 14, 16)
                    : gui_mkrc(0, 0, 0, 0);
                gui_rc pin_rc = show_pin
                    ? gui_mkrc(pr.x + pr.w - 34, pr.y + 5, 14, 16)
                    : gui_mkrc(0, 0, 0, 0);
                int hov = gui_hit(pr, ui->mx, ui->my);
                uint32_t bg = focused ? GUI_ACCENT_DARK
                            : hov ? GUI_PANEL_HI
                                  : gui_mix(GUI_SURFACE, GUI_PANEL, 128);
                uint32_t bd = focused ? GUI_ACCENT
                            : pinned ? GUI_WARN
                                     : (hov ? GUI_EDGE : GUI_BORDER);
                char ttl[20];
                int text_max;

                gui_fill_round_rect(win, pr, 7, bg);
                gui_stroke_round_rect(win, pr, 7, 1, bd);
                gui_icon(win, pr.x + 6, pr.y + 5, 16,
                         title_icon(t->title),
                         focused ? GUI_WHITE
                                 : (minimized ? GUI_TEXT_DIM : GUI_ACCENT));

                text_max = (show_pin ? pin_rc.x : (show_close ? close_rc.x
                                                              : pr.x + pr.w)) -
                           (pr.x + 26) - 2;
                if (text_max >= 12) {
                    task_title(t->title, ttl, text_max / 6 + 2);
                    gui_draw_text_aa_run(win, pr.x + 25, pr.y + 7, ttl, -1,
                                         FONT_SMALL,
                                         focused ? FONT_BOLD : FONT_REGULAR,
                                         FONT_ROMAN,
                                         focused ? GUI_WHITE
                                                 : (minimized ? GUI_TEXT_DIM
                                                              : GUI_TEXT),
                                         GUI_TRANSPARENT, text_max, 1);
                }

                if (show_pin) {
                    int phov = gui_hit(pin_rc, ui->mx, ui->my);
                    if (pinned || phov) {
                        gui_draw_pill(win, pin_rc,
                                      pinned ? GUI_WARN : GUI_PANEL,
                                      (long)(pinned ? GUI_WARN : GUI_EDGE));
                    }
                    gui_draw_text_aa(win, pin_rc.x + 4, pin_rc.y + 2, "P",
                                     FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                                     pinned ? GUI_TEXT_INV : GUI_TEXT_DIM,
                                     GUI_TRANSPARENT);
                }
                if (show_close) {
                    int chov = gui_hit(close_rc, ui->mx, ui->my);
                    if (chov)
                        gui_fill_round_rect(win, close_rc, 4, GUI_ERROR);
                    gui_draw_text_aa(win, close_rc.x + 4, close_rc.y + 2, "x",
                                     FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                                     chov ? GUI_WHITE : GUI_TEXT_DIM,
                                     GUI_TRANSPARENT);
                }

                if (minimized)
                    gui_rect(win, pr.x + 8, pr.y + pr.h - 2, pr.w - 16, 2,
                             GUI_TEXT_DIM);

                if (show_close && clicked(ui, close_rc)) {
                    win_ctl(t->wid, K_WIN_CTL_CLOSE);
                    refresh_tasks();
                } else if (show_pin && clicked(ui, pin_rc)) {
                    win_ctl(t->wid, K_WIN_CTL_TOGGLE_PIN);
                    refresh_tasks();
                    notify(pinned ? "Unpinned %s" : "Pinned %s across workspaces",
                           t->title);
                } else if (right_clicked(ui, pr)) {
                    win_ctl(t->wid, K_WIN_CTL_TOGGLE_PIN);
                    refresh_tasks();
                    notify(pinned ? "Unpinned %s" : "Pinned %s across workspaces",
                           t->title);
                } else if (clicked(ui, pr)) {
                    if (focused && !minimized)
                        win_ctl(t->wid, K_WIN_CTL_MINIMIZE);
                    else
                        win_ctl(t->wid, K_WIN_CTL_RESTORE);
                    refresh_tasks();
                }
                mid_x += pill_w;
            }
        }
    }
}

/* ----------------------------------------------------------- bottom dock */

static void paint_dock(gui_window *win, gui_ui *ui,
                       const gui_settings *settings)
{
    int bar_h = dock_h(settings);
    int y = win->h - bar_h;
    int item_h = settings->compact ? 36 : 44;
    int item_y = y + (bar_h - item_h) / 2;
    int x = 14;
    int status_w = 194;
    int hangar_w = 136;
    int max_x = win->w - status_w - hangar_w - 32;

    gui_rect(win, 0, y, win->w, bar_h, gui_mix(GUI_PANEL, GUI_BLACK, 20));
    gui_rect(win, 0, y, win->w, 1, GUI_EDGE);

    for (int i = 0; i < NAPPS; i++) {
        gui_rc r;
        int running;

        if (!apps[i].pinned_dock)
            continue;
        if (x + item_h + 8 > max_x)
            break;
        r = gui_mkrc(x, item_y, item_h, item_h);
        running = task_running_for_app(&apps[i]);
        {
            int hov = gui_hit(r, ui->mx, ui->my);
            uint32_t bg = hov ? GUI_PANEL_HI : GUI_SURFACE;
            uint32_t bd = running ? GUI_ACCENT : (hov ? GUI_EDGE : GUI_BORDER);

            gui_fill_round_rect(win, r, 10, bg);
            gui_stroke_round_rect(win, r, 10, 1, bd);
            gui_fill_round_rect(win, gui_mkrc(r.x + 5, r.y + 5,
                                              r.w - 10, r.h - 10),
                                7,
                                apps[i].enabled ? apps[i].color : GUI_TEXT_DIM);
            gui_icon(win, r.x + 9, r.y + 9, r.w - 18, apps[i].icon,
                     GUI_WHITE);
            if (running) {
                gui_fill_round_rect(win, gui_mkrc(r.x + r.w / 2 - 6,
                                                  y + bar_h - 4, 12, 3),
                                    1, GUI_ACCENT);
            }
            if (clicked(ui, r))
                launch_app(i);
            else if (running && right_clicked(ui, r)) {
                int ti = task_for_app(&apps[i]);
                if (ti >= 0)
                    park_task(&tasks[ti]);
            }
        }
        x += item_h + 8;
    }

    if (x + 120 < max_x) {
        gui_draw_text_aa(win, x + 8, y + (bar_h - 12) / 2,
                         "Super: Perch   Ctrl+T: Talon   Alt+1..4: WS",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    }

    hangar_btn_rc = gui_mkrc(win->w - status_w - hangar_w - 22, item_y,
                             hangar_w, item_h);
    if (gui_button_ex(win, hangar_btn_rc, 0, ui, 1,
                      popup_kind == POP_HANGAR)) {
        if (popup_kind == POP_HANGAR)
            close_popup();
        else
            open_popup(POP_HANGAR, win->w, win->h, settings);
    }
    {
        gui_rc hr = hangar_btn_rc;
        uint32_t fg = popup_kind == POP_HANGAR ? GUI_WHITE : GUI_ACCENT;
        int gx = hr.x + 12, gy = hr.y + (item_h - 18) / 2;
        char line[24];

        /* Hangar glyph: a bay roof over an arrow settling to the floor. */
        gui_line_aa(win, gx, gy + 7, gx + 9, gy + 1, fg);
        gui_line_aa(win, gx + 9, gy + 1, gx + 18, gy + 7, fg);
        gui_rect(win, gx, gy + 7, 2, 11, fg);
        gui_rect(win, gx + 16, gy + 7, 2, 11, fg);
        gui_rect(win, gx + 8, gy + 6, 2, 8, fg);
        gui_rect(win, gx + 6, gy + 11, 6, 2, fg);
        gui_rect(win, gx + 7, gy + 13, 4, 2, fg);
        gui_rect(win, gx + 3, gy + 16, 12, 2, fg);
        snprintf(line, sizeof(line), "Hangar  %d", nhangar);
        gui_draw_text_aa(win, hr.x + 38, hr.y + (item_h - 12) / 2, line,
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         popup_kind == POP_HANGAR ? GUI_WHITE : GUI_TEXT,
                         GUI_TRANSPARENT);
        if (nhangar > 0)
            gui_fill_circle(win, hr.x + hr.w - 14, hr.y + item_h / 2, 4,
                            0xF59E0BU);
    }

    quick_btn_rc = gui_mkrc(win->w - status_w - 12, item_y, status_w, item_h);
    if (gui_button_ex(win, quick_btn_rc, 0, ui, 1,
                      popup_kind == POP_QUICK)) {
        if (popup_kind == POP_QUICK)
            close_popup();
        else
            open_popup(POP_QUICK, win->w, win->h, settings);
    }
    {
        uint64_t um = 0, tm = 0;
        int mp = memory_percent(&um, &tm);
        char line[32];

        gui_icon(win, quick_btn_rc.x + 10,
                 quick_btn_rc.y + (item_h - 18) / 2,
                 18, GUI_ICON_SETTINGS,
                 popup_kind == POP_QUICK ? GUI_WHITE : GUI_ACCENT);
        snprintf(line, sizeof(line), "Control  %d%%", mp);
        gui_draw_text_aa(win, quick_btn_rc.x + 34,
                         quick_btn_rc.y + (item_h - 12) / 2,
                         line, FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         popup_kind == POP_QUICK ? GUI_WHITE : GUI_TEXT,
                         GUI_TRANSPARENT);
        gui_icon(win, quick_btn_rc.x + quick_btn_rc.w - 28,
                 quick_btn_rc.y + (item_h - 18) / 2,
                 18, GUI_ICON_POWER,
                 popup_kind == POP_QUICK ? GUI_WHITE : GUI_TEXT_DIM);
    }
}

static void paint_toast(gui_window *win)
{
    gui_rc r;
    int w;

    if (!toast[0] || uptime_ms() >= toast_until)
        return;
    w = gui_text_w(toast) + 58;
    if (w > 640)
        w = 640;
    r = gui_mkrc(win->w - w - 18, TOP_H + 14, w, 50);
    gui_shadow(win, r, 10, gui_mix(GUI_BORDER, GUI_DESK_BOTTOM, 80));
    gui_round_rect(win, r, 10, GUI_PANEL);
    gui_round_frame(win, r, 10, GUI_ACCENT);
    gui_round_rect(win, gui_mkrc(r.x + 10, r.y + 9, 32, 32), 8,
                   GUI_ACCENT_DARK);
    gui_icon(win, r.x + 18, r.y + 17, 16, GUI_ICON_CHECK, GUI_WHITE);
    gui_push_clip(win, gui_mkrc(r.x + 50, r.y, r.w - 58, r.h));
    gui_text(win, r.x + 50, r.y + 17, toast, GUI_TEXT, GUI_TRANSPARENT);
    gui_pop_clip(win);
}

static void paint_shell(gui_window *win, gui_ui *ui,
                        const gui_settings *settings)
{
    paint_topbar(win, ui);
    paint_dock(win, ui, settings);
    paint_toast(win);
}

/* ----------------------------------------------------------------- popups */

static void close_popup(void)
{
    if (popup)
        gui_close(popup);
    popup = 0;
    popup_kind = POP_NONE;
    power_confirm = -1;
}

static void open_popup(enum popup_kind kind, int sw, int sh,
                       const gui_settings *settings)
{
    gui_settings cur_s;
    int w, h, x, y, dh;
    const char *title = "Panel";

    if (!settings) {
        gui_settings_get(&cur_s);
        settings = &cur_s;
    }
    if (kind == POP_NONE) {
        close_popup();
        return;
    }
    close_popup();
    dh = dock_h(settings);

    if (kind == POP_LAUNCHER) {
        refresh_app_availability();
        refresh_tasks();
        w = sw - 28 < LAUNCH_W ? sw - 28 : LAUNCH_W;
        h = sh - TOP_H - dh - 20 < LAUNCH_H ? sh - TOP_H - dh - 20 : LAUNCH_H;
        x = 10;
        y = TOP_H + 6;
        title = "Kestrel Perch";
    } else if (kind == POP_TALON) {
        refresh_talon();
        w = sw - 28 < TALON_W ? sw - 28 : TALON_W;
        h = sh - TOP_H - dh - 20 < TALON_H ? sh - TOP_H - dh - 20 : TALON_H;
        x = sw - w - 10;
        y = TOP_H + 6;
        title = "Talon Visual Shelf";
    } else if (kind == POP_HANGAR) {
        refresh_hangar();
        w = sw - 28 < HANGAR_W ? sw - 28 : HANGAR_W;
        h = sh - TOP_H - dh - 20 < HANGAR_H ? sh - TOP_H - dh - 20 : HANGAR_H;
        x = (sw - w) / 2;
        y = sh - dh - h - 8;
        title = "Hangar";
    } else {
        w = sw - 28 < QUICK_W ? sw - 28 : QUICK_W;
        h = sh - TOP_H - dh - 20 < QUICK_H ? sh - TOP_H - dh - 20 : QUICK_H;
        x = sw - w - 10;
        y = sh - dh - h - 8;
        title = "Quick settings";
    }

    popup = gui_open(title, x, y, w, h, K_WIN_NODECOR);
    if (!popup) {
        notify("Could not open %s", title);
        return;
    }
    popup_kind = kind;
    memset(&popup_ui, 0, sizeof(popup_ui));
    perch_scroll = 0;
    perch_sel = 0;
    talon_scroll = 0;
    hangar_scroll = 0;
    hangar_confirm = -1;
    if (kind == POP_LAUNCHER) {
        search[0] = '\0';
        memset(&searchbox, 0, sizeof(searchbox));
        searchbox.buf = search;
        searchbox.cap = sizeof(search);
        searchbox.focus = 1;
    }
}

static void copy_eval_to_talon(const struct perch_eval *ev)
{
    const char *mime = ev->parcel_type == K_PARCEL_NUMBER ? "text/x-number"
                     : ev->parcel_type == K_PARCEL_COLOR  ? "text/x-color"
                                                          : "text/plain";
    if (parcel_put(ev->parcel_type, mime, ev->copy_label,
                   ev->copy_text, (uint32_t)strlen(ev->copy_text)) >= 0) {
        refresh_talon();
        notify("Copied %s to Talon", ev->copy_text);
    } else {
        notify("Failed to copy to Talon");
    }
}

/* Paint Kestrel Perch. Returns:
 *   >= 0  app index to launch
 *   -2    close popup
 *   -1    stay open */
static int paint_launcher(gui_window *win, gui_ui *ui)
{
    struct perch_eval evres;
    int win_matches[MAX_TASKS], nwin_matches = 0;
    int app_matches[NAPPS], app_scores[NAPPS], napp_matches = 0;
    int margin = 14, gap = 8;
    int cols = win->w >= 540 ? 3 : 2;
    int bar_w = 12;
    int view_x = margin, view_y = 92;
    int view_w = win->w - 2 * margin - bar_w - 4;
    int view_h = win->h - view_y - 28;
    int cell_w = (view_w - (cols - 1) * gap) / cols;
    int cur_y = 0, total_h = 0;
    int launch = -1;
    int activate_win = -1;
    int copy_eval = 0;
    int entered = 0;
    gui_rc search_rc = gui_mkrc(48, 48, win->w - 64, 32);

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 38),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 28), GUI_PANEL);
    gui_icon(win, 14, 9, 20, GUI_ICON_APPS, GUI_ACCENT);
    gui_draw_text_aa(win, 40, 11, "Kestrel Perch", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    if (gui_icon_button(win, gui_mkrc(win->w - 38, 6, 30, 26),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;

    gui_icon(win, 18, 53, 20, GUI_ICON_SEARCH, GUI_ACCENT);
    if (gui_textbox(win, search_rc, &searchbox, ui))
        entered = 1;

    eval_perch_query(search, &evres);

    /* Filter open windows across all workspaces when searching. */
    if (search[0]) {
        for (int i = 0; i < ntasks; i++) {
            if (fuzzy_score(tasks[i].title, search) > 0)
                win_matches[nwin_matches++] = i;
        }
    }

    /* Filter and sort apps by fuzzy score (enabled first on ties). */
    for (int i = 0; i < NAPPS; i++) {
        int sc = search[0]
            ? (fuzzy_score(apps[i].name, search) * 2 +
               fuzzy_score(apps[i].desc, search))
            : (apps[i].enabled ? 200 : 50);
        if (sc > 0) {
            app_matches[napp_matches] = i;
            app_scores[napp_matches] = sc + (apps[i].enabled ? 20 : 0);
            napp_matches++;
        }
    }
    for (int i = 1; i < napp_matches; i++) {
        int ai = app_matches[i], sc = app_scores[i], j = i - 1;
        while (j >= 0 && app_scores[j] < sc) {
            app_matches[j + 1] = app_matches[j];
            app_scores[j + 1] = app_scores[j];
            j--;
        }
        app_matches[j + 1] = ai;
        app_scores[j + 1] = sc;
    }

    if (entered) {
        if (evres.valid) {
            copy_eval = 1;
        } else if (nwin_matches > 0) {
            activate_win = win_matches[0];
        } else {
            for (int i = 0; i < napp_matches; i++) {
                if (apps[app_matches[i]].enabled) {
                    launch = app_matches[i];
                    break;
                }
            }
        }
    }

    /* Compute scrollable virtual content height first. */
    if (evres.valid || evres.is_error)
        total_h += 64;
    if (nwin_matches > 0)
        total_h += 22 + nwin_matches * 36 + 6;
    if (napp_matches > 0) {
        int rows = (napp_matches + cols - 1) / cols;
        total_h += 22 + rows * 68;
    } else if (!evres.valid && !evres.is_error && nwin_matches == 0) {
        total_h += 40;
    }

    if (ui->wheel && gui_hit(gui_mkrc(view_x, view_y, view_w + bar_w + 4,
                                      view_h), ui->mx, ui->my)) {
        perch_scroll -= ui->wheel * 34;
        ui->wheel = 0;
    }
    {
        int max_scroll = total_h - view_h;
        if (max_scroll < 0)
            max_scroll = 0;
        if (perch_scroll < 0)
            perch_scroll = 0;
        if (perch_scroll > max_scroll)
            perch_scroll = max_scroll;
        gui_scrollbar(win, gui_mkrc(win->w - margin - bar_w, view_y,
                                    bar_w, view_h),
                      total_h > 0 ? total_h : 1, view_h, &perch_scroll, ui);
    }

    gui_push_clip(win, gui_mkrc(view_x, view_y, view_w, view_h));
    cur_y = view_y - perch_scroll;

    /* 1. Inline Perch Evaluator card */
    if (evres.valid || evres.is_error) {
        gui_rc er = gui_mkrc(view_x, cur_y, view_w, 56);
        uint32_t bd = evres.is_error ? GUI_WARN : GUI_ACCENT;
        uint32_t bg = evres.is_error
            ? gui_mix(GUI_SURFACE_ALT, GUI_WARN, 24)
            : gui_mix(GUI_SURFACE_ALT, GUI_ACCENT, 26);

        gui_fill_round_rect(win, er, 8, bg);
        gui_stroke_round_rect(win, er, 8, 1, bd);
        if (evres.has_color) {
            gui_fill_round_rect(win, gui_mkrc(er.x + 12, er.y + 12, 32, 32),
                                7, evres.color_rgb);
            gui_stroke_round_rect(win, gui_mkrc(er.x + 12, er.y + 12, 32, 32),
                                  7, 1, GUI_EDGE);
        } else {
            gui_draw_pill(win, gui_mkrc(er.x + 12, er.y + 15, 44, 24),
                          evres.is_error ? GUI_WARN : GUI_ACCENT_DARK,
                          (long)bd);
            gui_draw_text_aa(win, er.x + 21, er.y + 21,
                             evres.is_error ? "ERR" : "EVAL",
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             GUI_WHITE, GUI_TRANSPARENT);
        }
        {
            int tx = er.x + (evres.has_color ? 56 : 68);
            gui_draw_text_aa_run(win, tx, er.y + 8, evres.title, -1,
                                 FONT_BODY, FONT_BOLD, FONT_ROMAN,
                                 GUI_TEXT, GUI_TRANSPARENT,
                                 er.w - (tx - er.x) - 96, 1);
            gui_draw_text_aa_run(win, tx, er.y + 31, evres.subtitle, -1,
                                 FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                 GUI_TEXT_DIM, GUI_TRANSPARENT,
                                 er.w - (tx - er.x) - 14, 1);
        }
        if (evres.valid) {
            gui_rc cp = gui_mkrc(er.x + er.w - 84, er.y + 12, 74, 22);
            gui_draw_pill(win, cp, GUI_ACCENT_DARK, (long)GUI_ACCENT);
            gui_draw_text_aa(win, cp.x + 8, cp.y + 5, "Enter Copy",
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             GUI_WHITE, GUI_TRANSPARENT);
            if (clicked(ui, er) && gui_hit(gui_mkrc(view_x, view_y,
                                                    view_w, view_h),
                                           ui->up_x, ui->up_y)) {
                copy_eval = 1;
            }
        }
        cur_y += 64;
    }

    /* 2. Matching open windows across workspaces */
    if (nwin_matches > 0) {
        gui_draw_text_aa(win, view_x, cur_y + 2,
                         "OPEN WINDOWS (CLICK TO SWITCH & FOCUS)",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
        cur_y += 22;
        for (int k = 0; k < nwin_matches; k++) {
            const struct k_winsummary *t = &tasks[win_matches[k]];
            int w_ws = (int)((t->state >> K_WIN_STATE_WS_SHIFT) &
                             K_WIN_STATE_WS_MASK);
            int pinned = (t->state & K_WIN_STATE_PINNED) != 0;
            gui_rc wr = gui_mkrc(view_x, cur_y, view_w, 32);
            int hov = gui_hit(wr, ui->mx, ui->my);
            char ws_s[16];

            gui_fill_round_rect(win, wr, 7,
                                hov ? GUI_PANEL_HI : GUI_SURFACE_ALT);
            gui_stroke_round_rect(win, wr, 7, 1,
                                  hov ? GUI_ACCENT : GUI_BORDER);
            gui_icon(win, wr.x + 8, wr.y + 7, 18, title_icon(t->title),
                     GUI_ACCENT);
            gui_draw_text_aa_run(win, wr.x + 32, wr.y + 8, t->title, -1,
                                 FONT_BODY, FONT_REGULAR, FONT_ROMAN,
                                 GUI_TEXT, GUI_TRANSPARENT,
                                 wr.w - 130, 1);
            snprintf(ws_s, sizeof(ws_s), pinned ? "PIN" : "WS %d", w_ws + 1);
            gui_draw_pill(win, gui_mkrc(wr.x + wr.w - 92, wr.y + 6, 46, 20),
                          pinned ? GUI_WARN : GUI_ACCENT_DARK,
                          (long)(pinned ? GUI_WARN : GUI_ACCENT));
            gui_draw_text_aa(win, wr.x + wr.w - 83, wr.y + 10, ws_s,
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             pinned ? GUI_TEXT_INV : GUI_WHITE,
                             GUI_TRANSPARENT);
            gui_draw_text_aa(win, wr.x + wr.w - 40, wr.y + 10, "Focus",
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT);
            if (clicked(ui, wr) && gui_hit(gui_mkrc(view_x, view_y,
                                                    view_w, view_h),
                                           ui->up_x, ui->up_y)) {
                activate_win = win_matches[k];
            }
            cur_y += 36;
        }
        cur_y += 6;
    }

    /* 3. Applications grid */
    if (napp_matches > 0) {
        gui_draw_text_aa(win, view_x, cur_y + 2,
                         search[0] ? "APPLICATIONS" : "ALL APPLICATIONS",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
        cur_y += 22;
        for (int k = 0; k < napp_matches; k++) {
            int idx = app_matches[k];
            int col = k % cols;
            int row = k / cols;
            gui_rc card = gui_mkrc(view_x + col * (cell_w + gap),
                                   cur_y + row * 68, cell_w, 60);
            gui_rc icon = gui_mkrc(card.x + 8, card.y + 10, 40, 40);
            int hov = gui_hit(card, ui->mx, ui->my);

            gui_fill_round_rect(win, card, 8,
                                hov && apps[idx].enabled
                                    ? GUI_PANEL_HI : GUI_SURFACE_ALT);
            gui_stroke_round_rect(win, card, 8, 1,
                                  hov && apps[idx].enabled
                                      ? GUI_ACCENT : GUI_EDGE);
            gui_round_rect(win, icon, 10,
                           apps[idx].enabled ? apps[idx].color : GUI_TEXT_DIM);
            gui_icon(win, icon.x + 8, icon.y + 8, 24, apps[idx].icon,
                     GUI_WHITE);
            gui_draw_text_aa_run(win, card.x + 56, card.y + 10, apps[idx].name,
                                 -1, FONT_BODY, FONT_BOLD, FONT_ROMAN,
                                 apps[idx].enabled ? GUI_TEXT : GUI_TEXT_DIM,
                                 GUI_TRANSPARENT, card.w - 64, 1);
            gui_draw_text_aa_run(win, card.x + 56, card.y + 33,
                                 apps[idx].enabled ? apps[idx].desc
                                                   : "Not installed",
                                 -1, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                 GUI_TEXT_DIM, GUI_TRANSPARENT,
                                 card.w - 64, 1);

            if (clicked(ui, card) &&
                gui_hit(gui_mkrc(view_x, view_y, view_w, view_h),
                        ui->up_x, ui->up_y)) {
                launch = idx;
            }
        }
    } else if (!evres.valid && !evres.is_error && nwin_matches == 0) {
        gui_draw_text_aa(win, view_x, cur_y + 10,
                         "No matching apps, windows, or expressions.",
                         FONT_BODY, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    }

    gui_pop_clip(win);

    gui_rect(win, 0, win->h - 24, win->w, 24, GUI_SUNKEN);
    gui_draw_text_aa(win, margin, win->h - 18,
                     "Tip: type =2+2*8, 0xFF, #60A5FA, or @krec and press Enter",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_TEXT_DIM, GUI_TRANSPARENT);

    if (copy_eval && evres.valid) {
        copy_eval_to_talon(&evres);
        return -2;
    }
    if (activate_win >= 0 && activate_win < ntasks) {
        win_ctl(tasks[activate_win].wid, K_WIN_CTL_RESTORE);
        refresh_tasks();
        return -2;
    }
    return launch;
}

/* ----------------------------------------------------- Talon Visual Shelf */

static const char *parcel_type_tag(uint32_t t, uint32_t *color_out)
{
    switch (t) {
    case K_PARCEL_URL:    *color_out = 0x3B82F6U; return "URL";
    case K_PARCEL_COLOR:  *color_out = 0xEC4899U; return "COLOR";
    case K_PARCEL_KREC:   *color_out = 0x8B5CF6U; return "KREC";
    case K_PARCEL_PATH:   *color_out = 0xF59E0BU; return "PATH";
    case K_PARCEL_NUMBER: *color_out = 0x14B8A6U; return "NUM";
    case K_PARCEL_IMAGE:  *color_out = 0x64748BU; return "IMG";
    case K_PARCEL_TEXT:
    default:              *color_out = 0x0EA5E9U; return "TEXT";
    }
}

static void promote_talon_slot(int idx)
{
    struct k_parcel_info info;
    char *buf;
    long got;
    int new_id;

    if (idx < 0 || idx >= ntalon)
        return;
    info = talon_slots[idx].info;
    if (info.size > K_PARCEL_MAX_BYTES)
        return;
    buf = malloc(info.size ? info.size : 1);
    if (!buf) {
        notify("Out of memory promoting parcel");
        return;
    }
    got = parcel_get(info.id, &info, buf, info.size);
    if (got < 0) {
        free(buf);
        notify("Parcel #%u no longer available", info.id);
        refresh_talon();
        return;
    }
    info.mime[sizeof(info.mime) - 1] = '\0';
    info.label[sizeof(info.label) - 1] = '\0';
    new_id = parcel_put(info.type, info.mime, info.label, buf, (uint32_t)got);
    free(buf);
    if (new_id >= 0) {
        refresh_talon();
        notify("Promoted %s to Talon slot 0 (#%d)",
               info.label[0] ? info.label : "parcel", new_id);
    } else {
        notify("Could not promote parcel #%u", info.id);
    }
}

static int paint_talon_shelf(gui_window *win, gui_ui *ui)
{
    int margin = 14, bar_w = 12, row_h = 54;
    int view_y = 48, view_h = win->h - view_y - 30;
    int view_w = win->w - 2 * margin - bar_w - 4;
    int total_h = ntalon > 0 ? ntalon * row_h : 60;
    int max_scroll = total_h - view_h;
    int clicked_slot = -1;
    char hdr_badge[24];

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 40),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 26), GUI_PANEL);
    gui_draw_text_aa(win, 14, 12, "Talon Visual Shelf", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    snprintf(hdr_badge, sizeof(hdr_badge), "%d / %d slots",
             ntalon, K_PARCEL_MAX_SLOTS);
    gui_badge(win, gui_mkrc(win->w - 134, 8, 86, 24), hdr_badge, GUI_ACCENT);
    if (gui_icon_button(win, gui_mkrc(win->w - 40, 7, 30, 26),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;

    if (max_scroll < 0)
        max_scroll = 0;
    if (ui->wheel && gui_hit(gui_mkrc(margin, view_y, view_w + bar_w + 4,
                                      view_h), ui->mx, ui->my)) {
        talon_scroll -= ui->wheel * 36;
        ui->wheel = 0;
    }
    if (talon_scroll < 0)
        talon_scroll = 0;
    if (talon_scroll > max_scroll)
        talon_scroll = max_scroll;

    gui_scrollbar(win, gui_mkrc(win->w - margin - bar_w, view_y,
                                bar_w, view_h),
                  total_h, view_h, &talon_scroll, ui);

    gui_push_clip(win, gui_mkrc(margin, view_y, view_w, view_h));
    if (ntalon == 0) {
        gui_card(win, gui_mkrc(margin, view_y + 4, view_w, 64));
        gui_draw_text_aa(win, margin + 14, view_y + 18,
                         "Talon clipboard is empty", FONT_BODY, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
        gui_draw_text_aa(win, margin + 14, view_y + 40,
                         "Copy text, colors, or Perch results to populate MRU",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    } else {
        for (int i = 0; i < ntalon; i++) {
            const struct talon_slot *ts = &talon_slots[i];
            int cy = view_y + i * row_h - talon_scroll;
            gui_rc card = gui_mkrc(margin, cy, view_w, row_h - 6);
            int hov = gui_hit(card, ui->mx, ui->my);
            uint32_t tcol = GUI_ACCENT;
            const char *ttag = parcel_type_tag(ts->info.type, &tcol);
            struct gui_tm tm;
            char meta[48], sz[16], tm_s[16];
            int tx;

            gui_fill_round_rect(win, card, 8,
                                i == 0 ? gui_mix(GUI_SURFACE_ALT, GUI_ACCENT, 22)
                                       : (hov ? GUI_PANEL_HI : GUI_SURFACE_ALT));
            gui_stroke_round_rect(win, card, 8, 1,
                                  i == 0 ? GUI_ACCENT
                                         : (hov ? GUI_EDGE : GUI_BORDER));

            /* Type pill */
            gui_draw_pill(win, gui_mkrc(card.x + 8, card.y + 12, 48, 22),
                          gui_mix(tcol, GUI_SURFACE, 150), (long)tcol);
            gui_draw_text_aa(win, card.x + 15, card.y + 17, ttag,
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             GUI_TEXT, GUI_TRANSPARENT);

            tx = card.x + 64;
            if (ts->has_color) {
                gui_rc sw = gui_mkrc(tx, card.y + 10, 28, 28);
                gui_fill_round_rect(win, sw, 6, ts->color_rgb);
                gui_stroke_round_rect(win, sw, 6, 1, GUI_EDGE);
                tx += 36;
            }

            if (ts->info.size < 1024)
                snprintf(sz, sizeof(sz), "%uB", ts->info.size);
            else
                snprintf(sz, sizeof(sz), "%uK", ts->info.size / 1024);
            if (ts->info.time > 0) {
                gui_gmtime(ts->info.time, &tm);
                snprintf(tm_s, sizeof(tm_s), "%02d:%02d:%02d",
                         tm.hour, tm.min, tm.sec);
            } else {
                snprintf(tm_s, sizeof(tm_s), "--:--");
            }
            snprintf(meta, sizeof(meta), "%s  %s  #%u", sz, tm_s, ts->info.id);

            int meta_w = gui_measure_text_aa(meta, FONT_SMALL, FONT_REGULAR,
                                             FONT_ROMAN, 0);
            gui_draw_text_aa_run(win, tx, card.y + 6,
                                 ts->info.label[0] ? ts->info.label : ttag,
                                 -1, FONT_BODY, FONT_BOLD, FONT_ROMAN,
                                 GUI_TEXT, GUI_TRANSPARENT,
                                 card.x + card.w - tx - meta_w - 14, 1);
            gui_draw_text_aa(win, card.x + card.w - meta_w - 8,
                             card.y + 8, meta, FONT_SMALL, FONT_REGULAR,
                             FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_aa_run(win, tx, card.y + 29,
                                 ts->preview[0] ? ts->preview : "(binary payload)",
                                 -1, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                 GUI_TEXT_DIM, GUI_TRANSPARENT,
                                 card.x + card.w - tx - 10, 1);

            if (clicked(ui, card) &&
                gui_hit(gui_mkrc(margin, view_y, view_w, view_h),
                        ui->up_x, ui->up_y)) {
                clicked_slot = i;
            }
        }
    }
    gui_pop_clip(win);

    gui_rect(win, 0, win->h - 24, win->w, 24, GUI_SUNKEN);
    gui_draw_text_aa(win, margin, win->h - 18,
                     "Click any parcel to promote it to active MRU slot 0",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_TEXT_DIM, GUI_TRANSPARENT);

    if (clicked_slot >= 0) {
        promote_talon_slot(clicked_slot);
        return -1;
    }
    return -1;
}

/* --------------------------------------------------------- Quick settings */

static int paint_quick(gui_window *win, gui_ui *ui, gui_settings *settings)
{
    struct gui_tm tm;
    struct k_cpuinfo cpu;
    uint64_t used_mib, total_mib;
    int mem_pct = memory_percent(&used_mib, &total_mib);
    char line[96], ip[16];
    int changed = 0;

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 42),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 24), GUI_PANEL);
    gui_draw_text_aa(win, 16, 13, "Quick Control", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    if (gui_icon_button(win, gui_mkrc(win->w - 40, 7, 32, 28),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;

    gui_gmtime(gui_time(), &tm);
    snprintf(line, sizeof(line), "%02d:%02d", tm.hour, tm.min);
    text_scaled(win, 18, 54, line, 2, GUI_TEXT);
    snprintf(line, sizeof(line), "%s, %d %s %d", gui_day_name(tm.wday),
             tm.day, gui_month_name(tm.mon), tm.year);
    gui_text(win, 20, 90, line, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_card(win, gui_mkrc(16, 118, win->w - 32, 72));
    gui_icon(win, 30, 132, 28, GUI_ICON_NETWORK,
             net_online ? GUI_OK : GUI_TEXT_DIM);
    gui_text(win, 70, 130,
             net_online ? "Network connected" : "Network offline",
             GUI_TEXT, GUI_TRANSPARENT);
    if (net_online) {
        ip_ntoa(net_cache.ip, ip);
        snprintf(line, sizeof(line), "%s  gateway available", ip);
    } else {
        snprintf(line, sizeof(line), "No configured network interface");
    }
    gui_text(win, 70, 154, line, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_text(win, 20, 206, "Memory", GUI_TEXT_DIM, GUI_TRANSPARENT);
    snprintf(line, sizeof(line), "%llu / %llu MiB",
             (unsigned long long)used_mib, (unsigned long long)total_mib);
    gui_text(win, win->w - gui_text_w(line) - 20, 206, line, GUI_TEXT,
             GUI_TRANSPARENT);
    gui_progress(win, gui_mkrc(20, 230, win->w - 40, 12), mem_pct,
                 mem_pct > 85 ? GUI_ERROR : GUI_ACCENT);

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    snprintf(line, sizeof(line), "%u CPU%s online", cpu.online,
             cpu.online == 1 ? "" : "s");
    gui_badge(win, gui_mkrc(20, 256, 136, 26), line, GUI_OK);
    snprintf(line, sizeof(line), "WS %d / %d", active_ws + 1,
             K_WM_MAX_WORKSPACES);
    gui_badge(win, gui_mkrc(166, 256, 92, 26), line, GUI_ACCENT);

    {
        int light = settings->light;
        int compact = settings->compact;

        if (gui_toggle(win, 20, 300, "Daylight mode", &light, ui)) {
            settings->light = light;
            settings->preset = light ? GUI_THEME_DAYLIGHT
                                     : GUI_THEME_AVIONICS_DARK;
            changed = 1;
        }
        if (gui_toggle(win, 196, 300, "Compact dock", &compact, ui)) {
            settings->compact = compact;
            changed = 1;
        }
    }

    if (changed) {
        if (gui_settings_save(settings) != 0)
            notify("Could not save desktop settings");
        else
            notify("Appearance updated");
    }

    if (power_confirm < 0) {
        if (gui_icon_button(win, gui_mkrc(20, win->h - 42, 126, 30),
                            GUI_ICON_SETTINGS, "Settings", ui, 1, 0)) {
            launch_app(app_index("/bin/settings"));
            return -2;
        }
        if (gui_icon_button(win,
                            gui_mkrc(win->w - 148, win->h - 42, 128, 30),
                            GUI_ICON_POWER, "Power", ui, 1, 0))
            power_confirm = K_POWER_REBOOT;
    } else {
        gui_text(win, 20, win->h - 37, "Restart or shut down?",
                 GUI_WARN, GUI_TRANSPARENT);
        if (gui_button(win, gui_mkrc(win->w - 226, win->h - 44, 66, 32),
                       "Restart", ui))
            syscall(SYS_POWER, K_POWER_REBOOT, 0, 0, 0);
        if (gui_button(win, gui_mkrc(win->w - 154, win->h - 44, 64, 32),
                       "Halt", ui))
            syscall(SYS_POWER, K_POWER_HALT, 0, 0, 0);
        if (gui_button(win, gui_mkrc(win->w - 84, win->h - 44, 64, 32),
                       "Cancel", ui))
            power_confirm = -1;
    }
    return -1;
}

/* ------------------------------------------------------------- Hangar */

static int paint_hangar(gui_window *win, gui_ui *ui)
{
    int margin = 14, bar_w = 12, gap = 10;
    int view_y = 64, view_h = win->h - view_y - 30;
    int view_w = win->w - 2 * margin - bar_w - 4;
    int card_w = (view_w - gap) / 2, card_h = 164, row_h = card_h + gap;
    int rows = (nhangar + 1) / 2;
    int total_h = rows > 0 ? rows * row_h : 80;
    int max_scroll = total_h - view_h;
    int sweep = (int)((uptime_ms() / 30) % (K_HANGAR_THUMB_H + 40)) - 20;
    int resume_idx = -1, discard_idx = -1;
    char badge[24];

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 40),
                  gui_mix(GUI_PANEL, 0xF59E0BU, 30), GUI_PANEL);
    gui_draw_text_aa(win, 14, 12, "Hangar", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    snprintf(badge, sizeof(badge), "%d / %d bays", nhangar, K_HANGAR_BAYS);
    gui_badge(win, gui_mkrc(win->w - 140, 8, 92, 24), badge, 0xF59E0BU);
    if (gui_icon_button(win, gui_mkrc(win->w - 40, 7, 30, 26),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;
    gui_draw_text_aa(win, 14, 44,
                     "Parked apps are sealed to disk with their memory, "
                     "registers, files and windows - even across reboots.",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM,
                     GUI_TRANSPARENT);

    if (max_scroll < 0)
        max_scroll = 0;
    if (ui->wheel && gui_hit(gui_mkrc(margin, view_y, view_w + bar_w + 4,
                                      view_h), ui->mx, ui->my)) {
        hangar_scroll -= ui->wheel * 48;
        ui->wheel = 0;
    }
    if (hangar_scroll < 0)
        hangar_scroll = 0;
    if (hangar_scroll > max_scroll)
        hangar_scroll = max_scroll;
    gui_scrollbar(win, gui_mkrc(win->w - margin - bar_w, view_y, bar_w,
                                view_h),
                  total_h, view_h, &hangar_scroll, ui);

    gui_push_clip(win, gui_mkrc(margin, view_y, view_w, view_h));
    if (nhangar == 0) {
        gui_card(win, gui_mkrc(margin, view_y + 4, view_w, 76));
        gui_draw_text_aa(win, margin + 14, view_y + 18,
                         "All bays are clear", FONT_BODY, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
        gui_draw_text_aa(win, margin + 14, view_y + 40,
                         "Park an app with the arrow button in its title bar,",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, margin + 14, view_y + 56,
                         "or right-click a running app in the dock.",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    }
    for (int i = 0; i < nhangar; i++) {
        const struct k_hangar_info *hb = &hangar_bays[i];
        int cx = margin + (i % 2) * (card_w + gap);
        int cy = view_y + (i / 2) * row_h - hangar_scroll;
        gui_rc card = gui_mkrc(cx, cy, card_w, card_h);
        gui_rc thumb = gui_mkrc(cx + 10, cy + 10, K_HANGAR_THUMB_W,
                                K_HANGAR_THUMB_H);
        gui_rc resume_rc = gui_mkrc(cx + 10, cy + card_h - 40, 112, 30);
        gui_rc drop_rc = gui_mkrc(cx + card_w - 40, cy + card_h - 40, 30, 30);
        int hov = gui_hit(card, ui->mx, ui->my);
        int tx = thumb.x + thumb.w + 12, tw = card.x + card.w - tx - 8;
        struct gui_tm tm;
        char line[48], bayname[12];

        if (cy + card_h < view_y || cy > view_y + view_h)
            continue;
        gui_fill_round_rect(win, card, 10, hov ? GUI_PANEL_HI
                                               : GUI_SURFACE_ALT);
        gui_stroke_round_rect(win, card, 10, 1, hov ? 0xF59E0BU : GUI_BORDER);

        /* The frozen window, with a slow sensor sweep across it. */
        if (hangar_has_thumb[i])
            gui_bitmap(win, thumb.x, thumb.y, thumb.w, thumb.h,
                       hangar_thumbs[i], K_HANGAR_THUMB_W);
        else
            gui_rect(win, thumb.x, thumb.y, thumb.w, thumb.h, GUI_SUNKEN);
        if (sweep >= 0 && sweep < thumb.h)
            gui_fill_rect_alpha(win, thumb.x, thumb.y + sweep, thumb.w, 2,
                                0xF59E0BU, 90);
        gui_frame(win, thumb.x - 1, thumb.y - 1, thumb.w + 2, thumb.h + 2,
                  GUI_EDGE);

        snprintf(bayname, sizeof(bayname), "BAY %02u", hb->bay);
        gui_badge(win, gui_mkrc(tx, cy + 10, 64, 22), bayname, 0xF59E0BU);
        gui_draw_text_aa_run(win, tx, cy + 38, hb->name, -1, FONT_BODY,
                             FONT_BOLD, FONT_ROMAN, GUI_TEXT,
                             GUI_TRANSPARENT, tw, 1);
        gui_draw_text_aa_run(win, tx, cy + 58,
                             hb->nwins ? hb->title : "(no window)", -1,
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT, tw, 1);
        gui_gmtime(hb->parked_at, &tm);
        snprintf(line, sizeof(line), "Parked %02d:%02d UTC", tm.hour, tm.min);
        gui_draw_text_aa(win, tx, cy + 76, line, FONT_SMALL, FONT_REGULAR,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        if (hb->bytes >= 1024 * 1024)
            snprintf(line, sizeof(line), "%lu.%lu MiB sealed",
                     (unsigned long)(hb->bytes >> 20),
                     (unsigned long)((hb->bytes & 0xFFFFF) * 10 >> 20));
        else
            snprintf(line, sizeof(line), "%lu KiB sealed",
                     (unsigned long)((hb->bytes + 1023) >> 10));
        gui_draw_text_aa(win, tx, cy + 92, line, FONT_SMALL, FONT_REGULAR,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_fill_circle(win, tx + 4, cy + 115, 3, 0xF59E0BU);
        gui_draw_text_aa(win, tx + 12, cy + 109, "COLD  -  SEALED",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN, 0xF59E0BU,
                         GUI_TRANSPARENT);

        if (gui_button_ex(win, resume_rc, "Resume", ui, 1, 0) &&
            gui_hit(gui_mkrc(margin, view_y, view_w, view_h),
                    ui->up_x, ui->up_y))
            resume_idx = i;
        if (hangar_confirm == (int)hb->bay)
            gui_draw_text_aa(win, resume_rc.x + resume_rc.w + 8,
                             resume_rc.y + 8, "Discard?", FONT_SMALL,
                             FONT_BOLD, FONT_ROMAN, 0xEF4444U,
                             GUI_TRANSPARENT);
        if (gui_icon_button(win, drop_rc, GUI_ICON_CLOSE, 0, ui, 1,
                            hangar_confirm == (int)hb->bay) &&
            gui_hit(gui_mkrc(margin, view_y, view_w, view_h),
                    ui->up_x, ui->up_y))
            discard_idx = i;
    }
    gui_pop_clip(win);

    gui_rect(win, 0, win->h - 24, win->w, 24, GUI_SUNKEN);
    gui_draw_text_aa(win, margin, win->h - 18,
                     "Apps with live network links, pipes or devices "
                     "will tell you why they cannot park",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM,
                     GUI_TRANSPARENT);

    if (resume_idx >= 0) {
        hangar_confirm = -1;
        return resume_bay(resume_idx) == 0 ? -2 : -1;
    }
    if (discard_idx >= 0) {
        int bay = (int)hangar_bays[discard_idx].bay;
        if (hangar_confirm != bay) {
            hangar_confirm = bay;
        } else {
            hangar_confirm = -1;
            if (hangar_discard(bay) == 0)
                notify("Hangar bay %02d cleared", bay);
            refresh_hangar();
        }
    }
    return -1;
}

static int paint_popup(gui_settings *settings)
{
    if (!popup)
        return -1;
    if (popup_kind == POP_LAUNCHER)
        return paint_launcher(popup, &popup_ui);
    if (popup_kind == POP_TALON)
        return paint_talon_shelf(popup, &popup_ui);
    if (popup_kind == POP_QUICK)
        return paint_quick(popup, &popup_ui, settings);
    if (popup_kind == POP_HANGAR)
        return paint_hangar(popup, &popup_ui);
    return -1;
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *desktop;
    gui_ui ui;
    gui_settings settings, painted_settings;
    int sw, sh, wallpaper_dirty = 1, shell_dirty = 1, popup_dirty = 0;
    unsigned long next_telemetry = 0, next_blink = 0;

    (void)argc;
    (void)argv;

    if (gui_screen(&sw, &sh) != 0) {
        printf("desktop: no framebuffer, nothing to draw on\n");
        return 1;
    }
    desktop = gui_open("Kestrel Desktop", 0, 0, sw, sh,
                       K_WIN_DESKTOP | K_WIN_NODECOR);
    if (!desktop) {
        printf("desktop: no window manager available\n");
        return 1;
    }

    refresh_app_availability();
    refresh_tasks();
    refresh_net();
    refresh_talon();
    refresh_hangar();
    init_pulses();
    sample_telemetry();

    memset(&ui, 0, sizeof(ui));
    gui_settings_get(&settings);
    painted_settings = settings;

    while (desktop->open) {
        unsigned long now;
        int got;

        gui_ui_begin(&ui);
        got = gui_next_event(desktop, &ev, popup ? 20 : 120);
        if (got < 0)
            break;
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_HANGAR) {
                paint_shell(desktop, &ui, &settings);
                gui_flush(desktop);
                park_window(ev.key);
                if (popup && popup_kind == POP_HANGAR)
                    popup_dirty = 1;
            }
            if (ev.type == KEV_KEY) {
                if (ev.key == KEY_LAUNCHER) {
                    open_popup(popup_kind == POP_LAUNCHER ? POP_NONE
                                                          : POP_LAUNCHER,
                               sw, sh, &settings);
                    popup_dirty = popup != 0;
                    shell_dirty = 1;
                } else if (ev.key >= KEY_WM_WS1 && ev.key <= KEY_WM_WS4) {
                    int ws = (int)(ev.key - KEY_WM_WS1);
                    if (win_ctl((uint32_t)ws, K_WIN_CTL_SWITCH_WS) == 0) {
                        active_ws = ws;
                        refresh_tasks();
                    }
                } else if (ev.key == 20) { /* Ctrl+T: Talon Shelf */
                    open_popup(popup_kind == POP_TALON ? POP_NONE : POP_TALON,
                               sw, sh, &settings);
                    popup_dirty = popup != 0;
                    shell_dirty = 1;
                } else if (ev.key == 17) { /* Ctrl+Q: Quick Control */
                    open_popup(popup_kind == POP_QUICK ? POP_NONE : POP_QUICK,
                               sw, sh, &settings);
                    popup_dirty = popup != 0;
                    shell_dirty = 1;
                }
            }
            if (popup && ev.type == KEV_MOUSE_DOWN &&
                !gui_hit(perch_btn_rc, ev.x, ev.y) &&
                !gui_hit(talon_btn_rc, ev.x, ev.y) &&
                !gui_hit(quick_btn_rc, ev.x, ev.y) &&
                !gui_hit(hangar_btn_rc, ev.x, ev.y)) {
                close_popup();
            }
            if (clicked(&ui, shortcut_rc(0)))
                launch_app(app_index("/bin/gfiles"));
            if (clicked(&ui, shortcut_rc(1)))
                launch_app(app_index("/bin/browser"));
            if (clicked(&ui, shortcut_rc(2)))
                launch_app(app_index("/bin/terminal"));
            shell_dirty = 1;
        }

        if (popup) {
            struct k_event pev;
            int pgot;

            gui_ui_begin(&popup_ui);
            pgot = gui_next_event(popup, &pev, 0);
            if (pgot < 0) {
                close_popup();
                shell_dirty = 1;
            } else if (pgot > 0) {
                gui_ui_event(&popup_ui, &pev);
                if (pev.type == KEV_CLOSE ||
                    (pev.type == KEV_KEY && pev.key == 27)) {
                    close_popup();
                    shell_dirty = 1;
                } else {
                    popup_dirty = 1;
                }
            }
        }

        poll_pulses(&settings, &wallpaper_dirty, &shell_dirty, &popup_dirty);

        now = uptime_ms();
        if (toast[0] && now >= toast_until) {
            toast[0] = '\0';
            toast_needs_background = 1;
        }
        if (toast_needs_background) {
            toast_needs_background = 0;
            wallpaper_dirty = 1;
            shell_dirty = 1;
        }

        if (now >= next_telemetry) {
            next_telemetry = now + 1000;
            reap_children();
            sample_telemetry();
            gui_settings_get(&settings);
            if (memcmp(&settings, &painted_settings, sizeof(settings)) != 0) {
                painted_settings = settings;
                wallpaper_dirty = 1;
                if (popup)
                    popup_dirty = 1;
            }
            shell_dirty = 1;
            if (popup && popup_kind == POP_QUICK)
                popup_dirty = 1;
        }

        if (popup && popup_kind == POP_LAUNCHER && now >= next_blink) {
            next_blink = now + 500;
            popup_dirty = 1;
        }
        if (popup && popup_kind == POP_HANGAR && now >= next_blink) {
            next_blink = now + 60;
            popup_dirty = 1;
        }

        if (wallpaper_dirty) {
            paint_wallpaper(desktop, &settings);
            wallpaper_dirty = 0;
            shell_dirty = 1;
        }
        if (shell_dirty) {
            paint_shell(desktop, &ui, &settings);
            if (gui_flush(desktop) != 0)
                break;
            shell_dirty = 0;
        }
        if (popup && popup_dirty) {
            int action = paint_popup(&settings);

            if (action == -2) {
                close_popup();
                shell_dirty = 1;
            } else if (action >= 0) {
                launch_app(action);
                close_popup();
                shell_dirty = 1;
            } else if (gui_flush(popup) != 0) {
                close_popup();
                shell_dirty = 1;
            }
            popup_dirty = 0;
        }
    }

    close_popup();
    gui_close(desktop);
    return 0;
}
