/* settings.c - desktop appearance and system preferences. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define WIN_W 700
#define WIN_H 510
#define NAV_W 168

enum settings_page {
    PAGE_APPEARANCE,
    PAGE_DESKTOP,
    PAGE_SYSTEM
};

static const uint32_t accent_color[6] = {
    0x60A5FAU, 0x8B5CF6U, 0x2DD4BFU,
    0xFB7185U, 0xFBBF24U, 0x4ADE80U
};

static int clicked(const gui_ui *ui, gui_rc r)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_LEFT) &&
           gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static void section_title(gui_window *win, int y, const char *title,
                          const char *description)
{
    gui_text(win, NAV_W + 28, y, title, GUI_TEXT, GUI_TRANSPARENT);
    gui_text(win, NAV_W + 28, y + 24, description, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
}

static void paint_nav(gui_window *win, gui_ui *ui, int *page)
{
    static const char *labels[3] = { "Appearance", "Desktop", "System" };
    static const enum gui_icon icons[3] = {
        GUI_ICON_PAINT, GUI_ICON_MONITOR, GUI_ICON_INFO
    };

    gui_rect(win, 0, 0, NAV_W, WIN_H, gui_mix(GUI_PANEL, GUI_BLACK, 14));
    gui_hgradient(win, gui_mkrc(0, 0, NAV_W, 56),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 32), GUI_PANEL);
    gui_icon(win, 18, 17, 24, GUI_ICON_SETTINGS, GUI_ACCENT);
    gui_text(win, 52, 21, "Settings", GUI_TEXT, GUI_TRANSPARENT);

    for (int i = 0; i < 3; i++) {
        gui_rc r = gui_mkrc(12, 76 + i * 48, NAV_W - 24, 38);

        if (gui_icon_button(win, r, icons[i], labels[i], ui, 1, i == *page))
            *page = i;
    }
    gui_text(win, 18, WIN_H - 46, "KestrelOS", GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_text(win, 18, WIN_H - 25, "Desktop preferences", GUI_TEXT_DIM,
             GUI_TRANSPARENT);
}

static void paint_preview(gui_window *win, const gui_settings *settings)
{
    gui_rc r = gui_mkrc(NAV_W + 28, 218, WIN_W - NAV_W - 56, 166);
    uint32_t base = settings->light ? 0xE8EEF8U : 0x111A2AU;
    uint32_t panel = settings->light ? 0xF3F6FBU : 0x151F2FU;
    uint32_t text = settings->light ? 0x172033U : 0xE9F0F8U;
    uint32_t dim = settings->light ? 0x65748AU : 0x91A0B5U;
    uint32_t accent = accent_color[settings->accent];

    gui_shadow(win, r, 10, GUI_BORDER);
    gui_round_rect(win, r, 10, base);
    gui_round_frame(win, r, 10, GUI_EDGE);
    gui_rect(win, r.x, r.y, r.w, 28, panel);
    gui_text(win, r.x + 12, r.y + 6, "Live preview", text,
             GUI_TRANSPARENT);
    gui_round_rect(win, gui_mkrc(r.x + 16, r.y + 48, 122, 82), 8, panel);
    gui_round_rect(win, gui_mkrc(r.x + 154, r.y + 48, r.w - 170, 34),
                   6, panel);
    gui_round_rect(win, gui_mkrc(r.x + 154, r.y + 94, 118, 30),
                   6, accent);
    gui_text(win, r.x + 169, r.y + 101, "Accent button",
             settings->accent == 4 ? 0x172033U : 0xFFFFFFU,
             GUI_TRANSPARENT);
    gui_text(win, r.x + 28, r.y + 61, "Panel", text, GUI_TRANSPARENT);
    gui_text(win, r.x + 28, r.y + 86, "Cards and", dim, GUI_TRANSPARENT);
    gui_text(win, r.x + 28, r.y + 105, "controls", dim, GUI_TRANSPARENT);
}

static void paint_appearance(gui_window *win, gui_ui *ui,
                             gui_settings *settings)
{
    int light = settings->light;

    section_title(win, 28, "Appearance",
                  "Choose a comfortable theme and accent colour.");
    gui_card(win, gui_mkrc(NAV_W + 28, 86, WIN_W - NAV_W - 56, 112));
    if (gui_toggle(win, NAV_W + 48, 110, "Use light appearance",
                   &light, ui))
        settings->light = light;
    gui_text(win, NAV_W + 48, 152, "Accent", GUI_TEXT_DIM,
             GUI_TRANSPARENT);

    for (int i = 0; i < 6; i++) {
        gui_rc hot = gui_mkrc(NAV_W + 122 + i * 48, 140, 34, 34);

        gui_disc(win, hot.x + 17, hot.y + 17, 13, accent_color[i]);
        if (i == settings->accent) {
            gui_circle(win, hot.x + 17, hot.y + 17, 16, GUI_TEXT);
            gui_icon(win, hot.x + 9, hot.y + 9, 16, GUI_ICON_CHECK,
                     i == 4 ? 0x172033U : GUI_WHITE);
        }
        if (clicked(ui, hot))
            settings->accent = i;
    }
    paint_preview(win, settings);
}

static void wallpaper_tile(gui_window *win, gui_ui *ui,
                           gui_settings *settings, int index, int x,
                           const char *label)
{
    gui_rc r = gui_mkrc(x, 112, 112, 108);
    uint32_t a = GUI_DESK_TOP, b = GUI_DESK_BOTTOM;

    if (index == 3) {
        a = gui_mix(a, 0xFB7185U, 90);
        b = gui_mix(b, 0x8B5CF6U, 70);
    }
    gui_round_rect(win, r, 8, GUI_PANEL);
    gui_vgradient(win, gui_mkrc(r.x + 6, r.y + 6, r.w - 12, 70), a, b);
    if (index == 0)
        gui_disc(win, r.x + 78, r.y + 30, 28,
                 gui_mix(a, GUI_ACCENT, 64));
    else if (index == 1)
        for (int i = 0; i < 4; i++)
            gui_line(win, r.x + 7, r.y + 30 + i * 12,
                     r.x + r.w - 7, r.y + 10 + i * 12, GUI_ACCENT);
    else if (index == 2)
        gui_round_frame(win, gui_mkrc(r.x + 28, r.y + 22, 56, 36),
                        8, gui_mix(a, GUI_TEXT, 80));
    else
        gui_disc(win, r.x + 70, r.y + 38, 28,
                 gui_mix(b, 0xFBBF24U, 70));
    gui_text(win, r.x + (r.w - gui_text_w(label)) / 2, r.y + 84,
             label, GUI_TEXT, GUI_TRANSPARENT);
    gui_round_frame(win, r, 8,
                    settings->wallpaper == index ? GUI_ACCENT : GUI_EDGE);
    if (clicked(ui, r))
        settings->wallpaper = index;
}

static void paint_desktop(gui_window *win, gui_ui *ui,
                          gui_settings *settings)
{
    static const char *names[4] = { "Aurora", "Contour", "Quiet", "Sunset" };
    int compact = settings->compact;

    section_title(win, 28, "Desktop",
                  "Select the wallpaper and dock density.");
    gui_text(win, NAV_W + 28, 84, "Wallpaper", GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    for (int i = 0; i < 4; i++)
        wallpaper_tile(win, ui, settings, i, NAV_W + 28 + i * 124, names[i]);

    gui_card(win, gui_mkrc(NAV_W + 28, 246, WIN_W - NAV_W - 56, 104));
    if (gui_toggle(win, NAV_W + 48, 272, "Use compact dock",
                   &compact, ui))
        settings->compact = compact;
    gui_text(win, NAV_W + 48, 306,
             "Compact mode keeps task controls visible on smaller displays.",
             GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_card(win, gui_mkrc(NAV_W + 28, 372, WIN_W - NAV_W - 56, 68));
    gui_text(win, NAV_W + 48, 390, "Keyboard shortcuts",
             GUI_TEXT, GUI_TRANSPARENT);
    gui_text(win, NAV_W + 48, 414,
             "Super opens apps   Alt-Tab switches   Alt-F4 closes",
             GUI_TEXT_DIM, GUI_TRANSPARENT);
}

static void value_row(gui_window *win, int y, const char *label,
                      const char *value)
{
    gui_text(win, NAV_W + 48, y, label, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_text(win, NAV_W + 198, y, value, GUI_TEXT, GUI_TRANSPARENT);
}

static void paint_system(gui_window *win, gui_ui *ui)
{
    struct k_cpuinfo cpu;
    struct k_netinfo net;
    struct k_fbinfo fb;
    uint64_t total = 0, free = 0, swap_total = 0, swap_used = 0;
    char buf[96], ip[16];
    int processes = 0;
    struct k_psinfo ps;

    section_title(win, 28, "System",
                  "Live hardware, memory and network information.");
    gui_card(win, gui_mkrc(NAV_W + 28, 86, WIN_W - NAV_W - 56, 278));

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    snprintf(buf, sizeof(buf), "%u online / %u discovered",
             cpu.online, cpu.discovered);
    value_row(win, 110, "Processors", buf);

    if (meminfo(&total, &free) == 0 && total) {
        uint64_t used = total - free;
        int pct = (int)((used * 100) / total);

        snprintf(buf, sizeof(buf), "%llu / %llu MiB",
                 (unsigned long long)(used / 1024),
                 (unsigned long long)(total / 1024));
        value_row(win, 142, "Memory", buf);
        gui_progress(win, gui_mkrc(NAV_W + 198, 166, 320, 10),
                     pct, pct > 85 ? GUI_ERROR : GUI_ACCENT);
    }

    swapinfo(&swap_total, &swap_used);
    snprintf(buf, sizeof(buf), "%llu / %llu MiB",
             (unsigned long long)(swap_used / 1024),
             (unsigned long long)(swap_total / 1024));
    value_row(win, 192, "Swap", buf);

    while (psinfo(processes, &ps) == 0)
        processes++;
    snprintf(buf, sizeof(buf), "%d running", processes);
    value_row(win, 224, "Processes", buf);

    memset(&fb, 0, sizeof(fb));
    syscall(SYS_FBINFO, (long)&fb, 0, 0, 0);
    snprintf(buf, sizeof(buf), "%ux%u at %u bpp", fb.width, fb.height, fb.bpp);
    value_row(win, 256, "Display", buf);

    if (netinfo(&net) == 0 && net.up)
        snprintf(buf, sizeof(buf), "%s", ip_ntoa(net.ip, ip));
    else
        snprintf(buf, sizeof(buf), "offline");
    value_row(win, 288, "Network", buf);

    snprintf(buf, sizeof(buf), "%luh %02lum",
             uptime_ms() / 3600000, (uptime_ms() / 60000) % 60);
    value_row(win, 320, "Uptime", buf);

    if (gui_icon_button(win, gui_mkrc(NAV_W + 28, 388, 180, 34),
                        GUI_ICON_MONITOR, "System Monitor", ui, 1, 0)) {
        char *argv[] = { "/bin/sysmon", 0 };
        spawn("/bin/sysmon", argv);
    }
    if (gui_icon_button(win, gui_mkrc(NAV_W + 220, 388, 142, 34),
                        GUI_ICON_INFO, "About", ui, 1, 0)) {
        char *argv[] = { "/bin/about", 0 };
        spawn("/bin/about", argv);
    }
}

static void paint(gui_window *win, gui_ui *ui, int *page,
                  gui_settings *settings, const gui_settings *saved,
                  char status[96])
{
    int changed;

    gui_clear(win, GUI_SURFACE);
    paint_nav(win, ui, page);
    if (*page == PAGE_APPEARANCE)
        paint_appearance(win, ui, settings);
    else if (*page == PAGE_DESKTOP)
        paint_desktop(win, ui, settings);
    else
        paint_system(win, ui);

    changed = memcmp(settings, saved, sizeof(*settings)) != 0;
    gui_rect(win, NAV_W, WIN_H - 52, WIN_W - NAV_W, 52, GUI_PANEL);
    gui_rect(win, NAV_W, WIN_H - 52, WIN_W - NAV_W, 1, GUI_EDGE);
    gui_text(win, NAV_W + 20, WIN_H - 34, status,
             changed ? GUI_WARN : GUI_TEXT_DIM, GUI_TRANSPARENT);
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    gui_settings settings, saved;
    char status[96] = "Changes apply to open apps.";
    int page = PAGE_APPEARANCE, dirty = 1;
    unsigned long next_system = 0;

    (void)argc;
    (void)argv;
    memset(&ui, 0, sizeof(ui));
    gui_settings_get(&settings);
    saved = settings;

    win = gui_open("Settings", 150, 90, WIN_W, WIN_H, 0);
    if (!win) {
        printf("settings: no window manager available\n");
        return 1;
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, 80);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == 27) {
                settings = saved;
                snprintf(status, sizeof(status), "Unsaved changes reverted.");
            }
            dirty = 1;
        }
        if (page == PAGE_SYSTEM && uptime_ms() >= next_system) {
            next_system = uptime_ms() + 1000;
            dirty = 1;
        }
        if (!dirty)
            continue;

        paint(win, &ui, &page, &settings, &saved, status);
        if (page != PAGE_SYSTEM) {
            int changed = memcmp(&settings, &saved, sizeof(settings)) != 0;

            if (gui_button_ex(win, gui_mkrc(WIN_W - 202, WIN_H - 42,
                                             84, 32),
                              "Reset", &ui, changed, 0)) {
                memset(&settings, 0, sizeof(settings));
                snprintf(status, sizeof(status), "Defaults selected.");
            }
            if (gui_button_ex(win, gui_mkrc(WIN_W - 108, WIN_H - 42,
                                             88, 32),
                              "Apply", &ui, changed, changed)) {
                if (gui_settings_save(&settings) == 0) {
                    saved = settings;
                    snprintf(status, sizeof(status),
                             "Appearance saved and applied.");
                } else {
                    snprintf(status, sizeof(status),
                             "Could not write your desktop preferences.");
                }
            }
        }
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_close(win);
    return 0;
}
