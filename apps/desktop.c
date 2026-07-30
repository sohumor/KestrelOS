/* desktop.c - the KestrelOS desktop environment.
 *
 * The full-screen K_WIN_DESKTOP surface owns the wallpaper, top panel, dock,
 * notifications and desktop shortcuts. Search and quick settings use a
 * temporary undecorated window: it sits above applications and can take
 * keyboard focus without turning the background layer into a normal window.
 *
 * The dock is backed by SYS_WIN_LIST/SYS_WIN_CTL, so its task buttons focus,
 * minimize and restore real compositor windows. The shell only receives
 * same-uid windows from the kernel.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define TOP_H          34
#define DOCK_H_FULL    66
#define DOCK_H_COMPACT 52
#define MAX_CHILDREN   32
#define MAX_TASKS      14
#define LAUNCH_W       620
#define LAUNCH_H       420
#define QUICK_W        390
#define QUICK_H        374

enum popup_kind {
    POP_NONE,
    POP_LAUNCHER,
    POP_QUICK
};

struct app_entry {
    const char *name;
    const char *desc;
    const char *path;
    enum gui_icon icon;
    uint32_t color;
    int enabled;
};

static struct app_entry apps[] = {
    { "Browser", "Explore the web", "/bin/browser", GUI_ICON_BROWSER,
      0x3B82F6U, 0 },
    { "Files", "Browse files and folders", "/bin/gfiles", GUI_ICON_FILES,
      0xF59E0BU, 0 },
    { "Terminal", "Command line and tools", "/bin/terminal",
      GUI_ICON_TERMINAL, 0x334155U, 0 },
    { "Paint", "Draw and edit KPX images", "/bin/gpaint", GUI_ICON_PAINT,
      0xEC4899U, 0 },
    { "Clock", "Time and calendar", "/bin/gclock", GUI_ICON_CLOCK,
      0x8B5CF6U, 0 },
    { "Calculator", "Quick integer calculations", "/bin/gcalc",
      GUI_ICON_CALCULATOR, 0x14B8A6U, 0 },
    { "System Monitor", "Processes and resources", "/bin/sysmon",
      GUI_ICON_MONITOR, 0x22C55EU, 0 },
    { "Settings", "Appearance and system preferences", "/bin/settings",
      GUI_ICON_SETTINGS, 0x64748BU, 0 },
    { "About", "KestrelOS system information", "/bin/about", GUI_ICON_INFO,
      0x0EA5E9U, 0 },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

static int children[MAX_CHILDREN];
static int nchildren;
static char toast[128];
static unsigned long toast_until;
static int toast_needs_background;

static gui_window *popup;
static enum popup_kind popup_kind;
static gui_ui popup_ui;
static char search[64];
static gui_textbox_state searchbox;
static int power_confirm = -1;

/* ----------------------------------------------------------------- helpers */

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

static int app_index(const char *path)
{
    for (int i = 0; i < NAPPS; i++)
        if (!strcmp(apps[i].path, path))
            return i;
    return -1;
}

static void launch_app(int index)
{
    char *argv[2];
    int pid;

    if (index < 0 || index >= NAPPS || !apps[index].enabled) {
        notify("%s is not installed", index >= 0 && index < NAPPS
                                      ? apps[index].name : "Application");
        return;
    }
    argv[0] = (char *)apps[index].path;
    argv[1] = 0;
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

static enum gui_icon title_icon(const char *title)
{
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

static void paint_shortcut(gui_window *win, int index, int app)
{
    gui_rc r = shortcut_rc(index);
    gui_rc icon = gui_mkrc(r.x + 12, r.y + 11, 48, 48);

    gui_shadow(win, r, 10, gui_mix(GUI_BORDER, GUI_DESK_BOTTOM, 80));
    gui_round_rect(win, r, 10, gui_mix(GUI_SURFACE, GUI_DESK_TOP, 70));
    gui_round_frame(win, r, 10, gui_mix(GUI_EDGE, GUI_DESK_TOP, 100));
    gui_round_rect(win, icon, 12, apps[app].color);
    gui_icon(win, icon.x + 10, icon.y + 10, 28, apps[app].icon, GUI_WHITE);
    gui_text(win, r.x + 72, r.y + 15, apps[app].name, GUI_TEXT,
             GUI_TRANSPARENT);
    gui_text(win, r.x + 72, r.y + 37,
             app == 1 ? "Home folder" : "Start browsing",
             GUI_TEXT_DIM, GUI_TRANSPARENT);
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
    gui_text(win, win->w - 418, h - 102,
             "A small system with a real desktop", GUI_TEXT_DIM,
             GUI_TRANSPARENT);

    paint_shortcut(win, 0, 1);
    paint_shortcut(win, 1, 0);
}

/* ------------------------------------------------------------------- shell */

static void paint_topbar(gui_window *win)
{
    char date[48];
    const char *workspace = "Desktop  1";

    gui_rect(win, 0, 0, win->w, TOP_H, gui_mix(GUI_PANEL, GUI_BLACK, 28));
    gui_rect(win, 0, TOP_H - 1, win->w, 1, GUI_EDGE);
    gui_icon(win, 12, 7, 20, GUI_ICON_APPS, GUI_ACCENT);
    gui_text(win, 40, 9, "Kestrel", GUI_TEXT, GUI_TRANSPARENT);
    gui_text(win, (win->w - gui_text_w(workspace)) / 2, 9, workspace,
             GUI_TEXT_DIM, GUI_TRANSPARENT);
    clock_text(date, sizeof(date), 1);
    gui_text(win, win->w - gui_text_w(date) - 16, 9, date, GUI_TEXT,
             GUI_TRANSPARENT);
}

static int collect_tasks(struct k_winsummary task[MAX_TASKS])
{
    int n = 0;

    while (n < MAX_TASKS && gui_window_list(n, &task[n]) == 0)
        n++;
    return n;
}

static void task_title(const char *src, char out[24], int max_chars)
{
    int i;

    if (max_chars < 1)
        max_chars = 1;
    for (i = 0; i < max_chars && i < 22 && src[i]; i++)
        out[i] = src[i];
    if (src[i] && i >= 3) {
        out[i - 3] = '.';
        out[i - 2] = '.';
        out[i - 1] = '.';
    }
    out[i] = '\0';
}

static void paint_toast(gui_window *win)
{
    gui_rc r;
    int w;

    if (!toast[0] || uptime_ms() >= toast_until) {
        return;
    }
    w = gui_text_w(toast) + 58;
    if (w > 430)
        w = 430;
    r = gui_mkrc(win->w - w - 18, TOP_H + 18, w, 52);
    gui_shadow(win, r, 10, gui_mix(GUI_BORDER, GUI_DESK_BOTTOM, 80));
    gui_round_rect(win, r, 10, GUI_PANEL);
    gui_round_frame(win, r, 10, GUI_EDGE);
    gui_round_rect(win, gui_mkrc(r.x + 10, r.y + 10, 32, 32), 8,
                   GUI_ACCENT_DARK);
    gui_icon(win, r.x + 18, r.y + 18, 16, GUI_ICON_CHECK, GUI_WHITE);
    gui_clip(win, gui_mkrc(r.x + 50, r.y, r.w - 58, r.h));
    gui_text(win, r.x + 50, r.y + 18, toast, GUI_TEXT, GUI_TRANSPARENT);
    gui_unclip(win);
}

static void close_popup(void);
static void open_popup(enum popup_kind kind, int sw, int sh,
                       const gui_settings *settings);

static void paint_dock(gui_window *win, gui_ui *ui,
                       const gui_settings *settings)
{
    struct k_winsummary task[MAX_TASKS];
    int bar_h = dock_h(settings);
    int y = win->h - bar_h;
    int item_h = settings->compact ? 36 : 46;
    int n = collect_tasks(task);
    int x = 12, task_x, status_w = 230;
    int avail, item_w;
    char clock[24];
    gui_rc apps_rc, status_rc;

    gui_rect(win, 0, y, win->w, bar_h, gui_mix(GUI_PANEL, GUI_BLACK, 20));
    gui_rect(win, 0, y, win->w, 1, GUI_EDGE);

    apps_rc = gui_mkrc(x, y + (bar_h - item_h) / 2, 50, item_h);
    if (gui_icon_button(win, apps_rc, GUI_ICON_APPS, 0, ui, 1,
                        popup_kind == POP_LAUNCHER))
        open_popup(popup_kind == POP_LAUNCHER ? POP_NONE : POP_LAUNCHER,
                   win->w, win->h, settings);
    x += 58;

    task_x = x;
    avail = win->w - task_x - status_w - 18;
    item_w = n > 0 ? avail / n : 0;
    if (item_w > 158)
        item_w = 158;
    if (item_w < 54 && n > 0)
        item_w = 54;

    for (int i = 0; i < n && task_x + item_w <= win->w - status_w - 8; i++) {
        int focused = (task[i].state & K_WIN_STATE_FOCUSED) != 0;
        int minimized = (task[i].state & K_WIN_STATE_MINIMIZED) != 0;
        gui_rc r = gui_mkrc(task_x, y + (bar_h - item_h) / 2,
                            item_w - 6, item_h);
        char title[24];
        const char *label = item_w >= 92 ? title : 0;

        task_title(task[i].title, title, (item_w - 42) / GUI_FONT_W);
        if (gui_icon_button(win, r, title_icon(task[i].title), label, ui,
                            1, focused)) {
            if (focused && !minimized)
                gui_window_control(task[i].wid, K_WIN_CTL_MINIMIZE);
            else
                gui_window_control(task[i].wid, K_WIN_CTL_RESTORE);
        }
        if (minimized)
            gui_rect(win, r.x + 8, r.y + r.h - 4, r.w - 16, 2,
                     GUI_TEXT_DIM);
        else if (focused)
            gui_round_rect(win, gui_mkrc(r.x + 10, r.y + r.h - 4,
                                         r.w - 20, 3), 2, GUI_ACCENT);
        task_x += item_w;
    }

    status_rc = gui_mkrc(win->w - status_w - 10,
                         y + (bar_h - item_h) / 2, status_w, item_h);
    if (gui_button_ex(win, status_rc, 0, ui, 1,
                      popup_kind == POP_QUICK))
        open_popup(popup_kind == POP_QUICK ? POP_NONE : POP_QUICK,
                   win->w, win->h, settings);

    {
        struct k_netinfo net;
        int online = netinfo(&net) == 0 && net.up;

        gui_icon(win, status_rc.x + 12, status_rc.y + (item_h - 20) / 2,
                 20, GUI_ICON_NETWORK, online ? GUI_OK : GUI_TEXT_DIM);
        gui_text(win, status_rc.x + 38,
                 status_rc.y + (item_h - GUI_FONT_H) / 2,
                 online ? "Online" : "Offline",
                 online ? GUI_TEXT : GUI_TEXT_DIM, GUI_TRANSPARENT);
    }
    clock_text(clock, sizeof(clock), 0);
    gui_text(win, status_rc.x + status_rc.w - gui_text_w(clock) - 14,
             status_rc.y + (item_h - GUI_FONT_H) / 2, clock, GUI_TEXT,
             GUI_TRANSPARENT);
}

static void paint_shell(gui_window *win, gui_ui *ui,
                        const gui_settings *settings)
{
    paint_topbar(win);
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
    int w, h, x, y;

    if (kind == POP_NONE) {
        close_popup();
        return;
    }
    close_popup();
    if (kind == POP_LAUNCHER) {
        w = sw - 36 < LAUNCH_W ? sw - 36 : LAUNCH_W;
        h = sh - TOP_H - dock_h(settings) - 30 < LAUNCH_H
                ? sh - TOP_H - dock_h(settings) - 30 : LAUNCH_H;
        x = 18;
    } else {
        w = sw - 36 < QUICK_W ? sw - 36 : QUICK_W;
        h = sh - TOP_H - dock_h(settings) - 30 < QUICK_H
                ? sh - TOP_H - dock_h(settings) - 30 : QUICK_H;
        x = sw - w - 18;
    }
    y = sh - dock_h(settings) - h - 10;
    popup = gui_open(kind == POP_LAUNCHER ? "Applications" : "Quick settings",
                     x, y, w, h, K_WIN_NODECOR);
    if (!popup) {
        notify("Could not open the desktop panel");
        return;
    }
    popup_kind = kind;
    memset(&popup_ui, 0, sizeof(popup_ui));
    if (kind == POP_LAUNCHER) {
        search[0] = '\0';
        memset(&searchbox, 0, sizeof(searchbox));
        searchbox.buf = search;
        searchbox.cap = sizeof(search);
        searchbox.focus = 1;
    }
}

static int first_search_result(void)
{
    for (int i = 0; i < NAPPS; i++)
        if (apps[i].enabled &&
            (contains_case(apps[i].name, search) ||
             contains_case(apps[i].desc, search)))
            return i;
    return -1;
}

static int paint_launcher(gui_window *win, gui_ui *ui)
{
    int cols = win->w >= 560 ? 3 : 2;
    int gap = 10, margin = 16;
    int cell_w = (win->w - 2 * margin - (cols - 1) * gap) / cols;
    int row = 0, shown = 0;
    int launch = -1;
    gui_rc search_rc = gui_mkrc(54, 52, win->w - 70, 34);

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 40),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 26), GUI_PANEL);
    gui_text(win, 16, 12, "Applications", GUI_TEXT, GUI_TRANSPARENT);
    if (gui_icon_button(win, gui_mkrc(win->w - 40, 6, 32, 28),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;

    gui_icon(win, 20, 59, 22, GUI_ICON_SEARCH, GUI_TEXT_DIM);
    if (gui_textbox(win, search_rc, &searchbox, ui))
        launch = first_search_result();
    gui_text(win, margin, 98, search[0] ? "Search results" : "All apps",
             GUI_TEXT_DIM, GUI_TRANSPARENT);

    for (int i = 0; i < NAPPS; i++) {
        int col;
        gui_rc card, icon;

        if (search[0] && !contains_case(apps[i].name, search) &&
            !contains_case(apps[i].desc, search))
            continue;
        col = shown % cols;
        row = shown / cols;
        card = gui_mkrc(margin + col * (cell_w + gap),
                        120 + row * 86, cell_w, 76);
        gui_card(win, card);
        icon = gui_mkrc(card.x + 10, card.y + 12, 48, 48);
        gui_round_rect(win, icon, 12,
                       apps[i].enabled ? apps[i].color : GUI_TEXT_DIM);
        gui_icon(win, icon.x + 10, icon.y + 10, 28, apps[i].icon,
                 GUI_WHITE);
        gui_text(win, card.x + 68, card.y + 14, apps[i].name,
                 apps[i].enabled ? GUI_TEXT : GUI_TEXT_DIM,
                 GUI_TRANSPARENT);
        gui_clip(win, gui_mkrc(card.x + 68, card.y + 34,
                               card.w - 76, 28));
        gui_text(win, card.x + 68, card.y + 37, apps[i].desc,
                 GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_unclip(win);
        if (clicked(ui, card) && apps[i].enabled)
            launch = i;
        shown++;
    }

    if (!shown)
        gui_text(win, margin, 142, "No applications match your search.",
                 GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_text(win, margin, win->h - 22,
             "Super: open launcher   Alt-Tab: switch   Alt-F4: close",
             GUI_TEXT_DIM, GUI_TRANSPARENT);
    return launch;
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

static int paint_quick(gui_window *win, gui_ui *ui, gui_settings *settings)
{
    struct gui_tm tm;
    struct k_netinfo net;
    struct k_cpuinfo cpu;
    uint64_t used_mib, total_mib;
    int mem_pct = memory_percent(&used_mib, &total_mib);
    int online = netinfo(&net) == 0 && net.up;
    char line[96], ip[16];
    int changed = 0;

    gui_clear(win, GUI_PANEL);
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 44),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 24), GUI_PANEL);
    gui_text(win, 16, 14, "Quick settings", GUI_TEXT, GUI_TRANSPARENT);
    if (gui_icon_button(win, gui_mkrc(win->w - 40, 8, 32, 28),
                        GUI_ICON_CLOSE, 0, ui, 1, 0))
        return -2;

    gui_gmtime(gui_time(), &tm);
    snprintf(line, sizeof(line), "%02d:%02d", tm.hour, tm.min);
    text_scaled(win, 18, 58, line, 2, GUI_TEXT);
    snprintf(line, sizeof(line), "%s, %d %s %d", gui_day_name(tm.wday),
             tm.day, gui_month_name(tm.mon), tm.year);
    gui_text(win, 20, 94, line, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_card(win, gui_mkrc(16, 122, win->w - 32, 72));
    gui_icon(win, 30, 136, 28, GUI_ICON_NETWORK,
             online ? GUI_OK : GUI_TEXT_DIM);
    gui_text(win, 70, 134, online ? "Network connected" : "Network offline",
             GUI_TEXT, GUI_TRANSPARENT);
    if (online) {
        ip_ntoa(net.ip, ip);
        snprintf(line, sizeof(line), "%s  gateway available", ip);
    } else {
        snprintf(line, sizeof(line), "No configured network interface");
    }
    gui_text(win, 70, 158, line, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_text(win, 20, 210, "Memory", GUI_TEXT_DIM, GUI_TRANSPARENT);
    snprintf(line, sizeof(line), "%llu / %llu MiB",
             (unsigned long long)used_mib, (unsigned long long)total_mib);
    gui_text(win, win->w - gui_text_w(line) - 20, 210, line, GUI_TEXT,
             GUI_TRANSPARENT);
    gui_progress(win, gui_mkrc(20, 234, win->w - 40, 12), mem_pct,
                 mem_pct > 85 ? GUI_ERROR : GUI_ACCENT);

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    snprintf(line, sizeof(line), "%u CPU%s online", cpu.online,
             cpu.online == 1 ? "" : "s");
    gui_badge(win, gui_mkrc(20, 260, 136, 26), line, GUI_OK);

    {
        int light = settings->light;
        int compact = settings->compact;

        if (gui_toggle(win, 20, 302, "Light appearance", &light, ui)) {
            settings->light = light;
            changed = 1;
        }
        if (gui_toggle(win, 196, 302, "Compact dock", &compact, ui)) {
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
            int idx = app_index("/bin/settings");
            launch_app(idx);
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

static int paint_popup(gui_settings *settings)
{
    if (!popup)
        return -1;
    if (popup_kind == POP_LAUNCHER)
        return paint_launcher(popup, &popup_ui);
    if (popup_kind == POP_QUICK)
        return paint_quick(popup, &popup_ui, settings);
    return -1;
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct k_event ev;
    struct k_stat st;
    gui_window *desktop;
    gui_ui ui;
    gui_settings settings, painted_settings;
    int sw, sh, wallpaper_dirty = 1, shell_dirty = 1, popup_dirty = 0;
    unsigned long next_tick = 0;

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
    for (int i = 0; i < NAPPS; i++)
        apps[i].enabled = stat_(apps[i].path, &st) == 0 && !st.is_dir;

    memset(&ui, 0, sizeof(ui));
    gui_settings_get(&settings);
    painted_settings = settings;

    while (desktop->open) {
        int got;

        gui_ui_begin(&ui);
        got = gui_next_event(desktop, &ev, popup ? 20 : 80);
        if (got < 0)
            break;
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == KEY_LAUNCHER) {
                open_popup(popup_kind == POP_LAUNCHER ? POP_NONE
                                                     : POP_LAUNCHER,
                           sw, sh, &settings);
                popup_dirty = popup != 0;
                shell_dirty = 1;
            }
            if (popup && ev.type == KEV_MOUSE_DOWN)
                close_popup();
            if (clicked(&ui, shortcut_rc(0)))
                launch_app(1);
            if (clicked(&ui, shortcut_rc(1)))
                launch_app(0);
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

        if (uptime_ms() >= next_tick) {
            next_tick = uptime_ms() + 500;
            reap_children();
            if (toast[0] && uptime_ms() >= toast_until) {
                toast[0] = '\0';
                toast_needs_background = 1;
            }
            if (toast_needs_background) {
                toast_needs_background = 0;
                wallpaper_dirty = 1;
            }
            gui_settings_get(&settings);
            if (memcmp(&settings, &painted_settings,
                       sizeof(settings)) != 0) {
                painted_settings = settings;
                wallpaper_dirty = 1;
                if (popup)
                    close_popup();
            }
            shell_dirty = 1;
            if (popup)
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
            } else {
                if (action >= 0) {
                    launch_app(action);
                    close_popup();
                    shell_dirty = 1;
                } else if (gui_flush(popup) != 0) {
                    close_popup();
                    shell_dirty = 1;
                }
            }
            popup_dirty = 0;
        }
    }

    close_popup();
    gui_close(desktop);
    return 0;
}
