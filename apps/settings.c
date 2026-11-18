/* settings.c - Avionics Theme & Workspace Control Center. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define DEF_W 740
#define DEF_H 540
#define NAV_W 178

enum settings_page {
    PAGE_APPEARANCE = 0,
    PAGE_DESKTOP    = 1,
    PAGE_SYSTEM     = 2
};

static const uint32_t accent_color[6] = {
    0x60A5FAU, 0x8B5CF6U, 0x2DD4BFU,
    0xFB7185U, 0xFBBF24U, 0x4ADE80U
};

struct preset_meta {
    int id;
    const char *name;
    const char *tag;
    int default_accent;
    uint32_t desk_top;
    uint32_t panel;
    uint32_t surface;
    uint32_t text;
    uint32_t dim;
    uint32_t edge;
};

static const struct preset_meta presets[4] = {
    { GUI_THEME_AVIONICS_DARK, "Avionics Dark", "Slate HUD", 0,
      0x0C1320U, 0x111A28U, 0x152031U, 0xEDF3FAU, 0x8FA1BAU, 0x344763U },
    { GUI_THEME_DAYLIGHT, "Daylight", "Paper Light", 0,
      0xE5ECF6U, 0xF4F7FCU, 0xEDF2F8U, 0x131C2EU, 0x5A6A82U, 0xBDCADDU },
    { GUI_THEME_AMBER, "Cockpit Amber", "Night Flight", 4,
      0x16110AU, 0x1C160EU, 0x211A10U, 0xFFF1D6U, 0xB89B6EU, 0x544128U },
    { GUI_THEME_BOREALIS, "Borealis", "Polar Teal", 2,
      0x0A1A1FU, 0x0E2228U, 0x11282FU, 0xE6F7F7U, 0x7EB3BAU, 0x295663U },
};

static int clicked(const gui_ui *ui, gui_rc r)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_LEFT) &&
           gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static int count_talon_parcels(struct k_parcel_info *slot0)
{
    struct k_parcel_info info;
    int n = 0;

    for (int i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
        if (parcel_list(i, &info) != 0)
            break;
        if (i == 0 && slot0) {
            *slot0 = info;
            slot0->label[sizeof(slot0->label) - 1] = '\0';
        }
        n++;
    }
    return n;
}

static const struct preset_meta *preset_for_settings(const gui_settings *s)
{
    if (s->light || s->preset == GUI_THEME_DAYLIGHT)
        return &presets[1];
    if (s->preset == GUI_THEME_AMBER)
        return &presets[2];
    if (s->preset == GUI_THEME_BOREALIS)
        return &presets[3];
    return &presets[0];
}

static void section_title(gui_window *win, int y, const char *title,
                          const char *description)
{
    gui_draw_text_aa(win, NAV_W + 24, y, title, FONT_LARGE, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    gui_draw_text_aa(win, NAV_W + 24, y + 26, description, FONT_SMALL,
                     FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
}

static void paint_nav(gui_window *win, gui_ui *ui, int *page, int talon_n)
{
    static const char *labels[3] = { "Appearance", "Desktop", "System" };
    static const enum gui_icon icons[3] = {
        GUI_ICON_PAINT, GUI_ICON_MONITOR, GUI_ICON_INFO
    };
    int h = win->h;
    struct k_cpuinfo cpu;
    uint64_t total = 0, free_kb = 0, used = 0;
    char line[48];

    gui_rect(win, 0, 0, NAV_W, h, gui_mix(GUI_PANEL, GUI_BLACK, 14));
    gui_rect(win, NAV_W - 1, 0, 1, h, GUI_EDGE);
    gui_hgradient(win, gui_mkrc(0, 0, NAV_W - 1, 58),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 34), GUI_PANEL);
    gui_icon(win, 16, 17, 24, GUI_ICON_SETTINGS, GUI_ACCENT);
    gui_draw_text_aa(win, 48, 20, "Settings", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);

    for (int i = 0; i < 3; i++) {
        gui_rc r = gui_mkrc(12, 74 + i * 46, NAV_W - 24, 38);

        if (gui_icon_button(win, r, icons[i], labels[i], ui, 1, i == *page))
            *page = i;
    }

    if (h >= 340) {
        gui_rc sum = gui_mkrc(12, h - 134, NAV_W - 24, 86);

        gui_fill_round_rect(win, sum, 8, GUI_SUNKEN);
        gui_stroke_round_rect(win, sum, 8, 1, GUI_BORDER);

        memset(&cpu, 0, sizeof(cpu));
        cpuinfo(&cpu);
        if (meminfo(&total, &free_kb) == 0 && total > 0)
            used = total - free_kb;

        snprintf(line, sizeof(line), "%u CPU  |  %llu MiB",
                 cpu.online, (unsigned long long)(used / 1024));
        gui_draw_text_aa(win, sum.x + 10, sum.y + 10, line, FONT_SMALL,
                         FONT_BOLD, FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);

        snprintf(line, sizeof(line), "Workspaces: %d", K_WM_MAX_WORKSPACES);
        gui_draw_text_aa(win, sum.x + 10, sum.y + 30, line, FONT_SMALL,
                         FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT);

        snprintf(line, sizeof(line), "Talon parcels: %d", talon_n);
        gui_draw_text_aa(win, sum.x + 10, sum.y + 48, line, FONT_SMALL,
                         FONT_REGULAR, FONT_ROMAN, GUI_ACCENT,
                         GUI_TRANSPARENT);

        snprintf(line, sizeof(line), "RAM: %llu / %lluM",
                 (unsigned long long)(used / 1024),
                 (unsigned long long)(total / 1024));
        gui_draw_text_aa(win, sum.x + 10, sum.y + 66, line, FONT_SMALL,
                         FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT);
    }

    gui_draw_text_aa(win, 16, h - 38, "KestrelOS Avionics", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, 16, h - 22, "Live theme & WM control", FONT_SMALL,
                     FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
}

/* 48x16 rounded swatch strip representing a preset's palette. */
static void paint_swatch_48x16(gui_window *win, int x, int y, uint32_t desk,
                               uint32_t panel, uint32_t surf, uint32_t accent,
                               uint32_t border)
{
    gui_rc box = gui_mkrc(x, y, 48, 16);

    gui_fill_round_rect(win, box, 5, desk);
    gui_fill_round_rect(win, gui_mkrc(x + 12, y + 2, 11, 12), 2, panel);
    gui_fill_round_rect(win, gui_mkrc(x + 24, y + 2, 11, 12), 2, surf);
    gui_fill_round_rect(win, gui_mkrc(x + 36, y + 2, 10, 12), 3, accent);
    gui_stroke_round_rect(win, box, 5, 1, border);
}

static void paint_preview(gui_window *win, const gui_settings *settings)
{
    const struct preset_meta *pm = preset_for_settings(settings);
    int content_x = NAV_W + 24;
    int content_w = win->w - NAV_W - 48;
    int preview_y = 266;
    int preview_h = win->h - preview_y - 66;
    gui_rc r;
    uint32_t accent = accent_color[settings->accent % 6];
    static const int demo_sp[10] = { 18, 28, 24, 42, 38, 56, 48, 66, 62, 74 };

    if (preview_h < 120)
        preview_h = 120;
    r = gui_mkrc(content_x, preview_y, content_w, preview_h);

    gui_draw_shadow(win, r, 10, 4, GUI_BORDER);
    gui_fill_round_rect(win, r, 10, pm->desk_top);
    gui_stroke_round_rect(win, r, 10, 1, pm->edge);

    /* Mockup titlebar */
    gui_fill_round_rect(win, gui_mkrc(r.x + 1, r.y + 1, r.w - 2, 28), 9,
                        pm->panel);
    gui_rect(win, r.x + 1, r.y + 18, r.w - 2, 11, pm->panel);
    gui_rect(win, r.x + 1, r.y + 28, r.w - 2, 2, accent);

    gui_draw_text_aa(win, r.x + 14, r.y + 7, "Avionics Window Preview",
                     FONT_SMALL, FONT_BOLD, FONT_ROMAN, pm->text,
                     GUI_TRANSPARENT);
    paint_swatch_48x16(win, r.x + r.w - 100, r.y + 6, pm->desk_top, pm->panel,
                       pm->surface, accent, pm->edge);
    gui_disc(win, r.x + r.w - 38, r.y + 14, 4, pm->dim);
    gui_disc(win, r.x + r.w - 26, r.y + 14, 4, pm->dim);
    gui_disc(win, r.x + r.w - 14, r.y + 14, 4, 0xFB7185U);

    /* Left mini panel inside mockup */
    {
        int left_w = r.w > 340 ? 136 : 104;
        gui_rc card_l = gui_mkrc(r.x + 14, r.y + 42, left_w, r.h - 56);
        gui_rc card_r = gui_mkrc(r.x + 26 + left_w, r.y + 42,
                                 r.w - 40 - left_w, r.h - 56);

        gui_fill_round_rect(win, card_l, 8, pm->panel);
        gui_stroke_round_rect(win, card_l, 8, 1, pm->edge);
        gui_draw_text_aa(win, card_l.x + 12, card_l.y + 10, pm->name,
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN, pm->text,
                         GUI_TRANSPARENT);
        gui_draw_text_aa(win, card_l.x + 12, card_l.y + 26, pm->tag,
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN, pm->dim,
                         GUI_TRANSPARENT);
        if (card_l.h >= 86) {
            gui_draw_sparkline(win,
                               gui_mkrc(card_l.x + 10, card_l.y + 48,
                                        card_l.w - 20, card_l.h - 60),
                               demo_sp, 10, 100, accent, (long)accent);
        }

        gui_fill_round_rect(win, card_r, 8, pm->surface);
        gui_stroke_round_rect(win, card_r, 8, 1, pm->edge);
        gui_draw_text_aa(win, card_r.x + 14, card_r.y + 12,
                         "Anti-aliased Avionics typography",
                         FONT_BODY, FONT_BOLD, FONT_ROMAN, pm->text,
                         GUI_TRANSPARENT);
        gui_draw_text_aa(win, card_r.x + 14, card_r.y + 36,
                         "Broadcasts live to compositor + all windows",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN, pm->dim,
                         GUI_TRANSPARENT);

        if (card_r.h >= 88) {
            gui_rc pill = gui_mkrc(card_r.x + 14, card_r.y + 60, 108, 26);
            gui_rc pill2 = gui_mkrc(card_r.x + 132, card_r.y + 60, 86, 26);
            uint32_t btn_fg = settings->accent == 4 ? 0x172033U : 0xFFFFFFU;

            gui_draw_pill(win, pill, accent, (long)accent);
            gui_draw_text_aa(win, pill.x + 14, pill.y + 7, "Accent Pill",
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN, btn_fg,
                             GUI_TRANSPARENT);
            gui_draw_pill(win, pill2, pm->panel, (long)pm->edge);
            gui_draw_text_aa(win, pill2.x + 16, pill2.y + 7, "WS 1-4",
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN, pm->text,
                             GUI_TRANSPARENT);
        }
    }
}

static void paint_appearance(gui_window *win, gui_ui *ui,
                             gui_settings *settings, gui_settings *saved,
                             char status[96])
{
    int content_x = NAV_W + 24;
    int content_w = win->w - NAV_W - 48;
    int gap = 10;
    int tile_w = (content_w - 3 * gap) / 4;
    int cur_preset = settings->light ? GUI_THEME_DAYLIGHT : settings->preset;
    int light = settings->light;

    section_title(win, 20, "Appearance",
                  "Select an Avionics theme preset and accent colour.");

    /* 4 Named Avionics Preset Cards */
    for (int i = 0; i < 4; i++) {
        const struct preset_meta *pm = &presets[i];
        gui_rc pr = gui_mkrc(content_x + i * (tile_w + gap), 74, tile_w, 72);
        int active = (cur_preset == pm->id);
        int hov = gui_hit(pr, ui->mx, ui->my);

        gui_fill_round_rect(win, pr, 9,
                            active ? gui_mix(GUI_SURFACE_ALT, GUI_ACCENT, 26)
                                   : (hov ? GUI_PANEL_HI : GUI_SURFACE_ALT));
        gui_stroke_round_rect(win, pr, 9, active ? 2 : 1,
                              active ? GUI_ACCENT
                                     : (hov ? GUI_EDGE : GUI_BORDER));

        paint_swatch_48x16(win, pr.x + 10, pr.y + 10, pm->desk_top, pm->panel,
                           pm->surface, accent_color[pm->default_accent],
                           pm->edge);
        if (active) {
            gui_disc(win, pr.x + pr.w - 16, pr.y + 18, 7, GUI_ACCENT);
            gui_icon(win, pr.x + pr.w - 22, pr.y + 12, 12,
                     GUI_ICON_CHECK, GUI_WHITE);
        }
        gui_draw_text_aa_run(win, pr.x + 10, pr.y + 34, pm->name, -1,
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             GUI_TEXT, GUI_TRANSPARENT, pr.w - 18, 1);
        gui_draw_text_aa_run(win, pr.x + 10, pr.y + 50, pm->tag, -1,
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT, pr.w - 18, 1);

        if (clicked(ui, pr)) {
            gui_settings applied;

            gui_theme_set_preset(pm->id);
            gui_settings_get(&applied);
            settings->preset = applied.preset;
            settings->light = applied.light;
            settings->accent = applied.accent;
            if (gui_settings_save(settings) == 0) {
                *saved = *settings;
                snprintf(status, 96, "%s applied across desktop & apps.",
                         pm->name);
            } else {
                snprintf(status, 96, "Preset previewed (save failed).");
            }
        }
    }

    /* Accent Swatches + Light Appearance Card */
    {
        gui_rc ac_card = gui_mkrc(content_x, 158, content_w, 94);

        gui_card(win, ac_card);
        if (gui_toggle(win, ac_card.x + 18, ac_card.y + 14,
                       "Use Daylight (light) palette", &light, ui)) {
            settings->light = light;
            if (light)
                settings->preset = GUI_THEME_DAYLIGHT;
            else if (settings->preset == GUI_THEME_DAYLIGHT)
                settings->preset = GUI_THEME_AVIONICS_DARK;
        }
        gui_draw_text_aa(win, ac_card.x + 18, ac_card.y + 54, "Accent",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT);

        for (int i = 0; i < 6; i++) {
            gui_rc hot = gui_mkrc(ac_card.x + 86 + i * 46, ac_card.y + 44,
                                  34, 34);

            gui_disc(win, hot.x + 17, hot.y + 17, 13, accent_color[i]);
            if (i == settings->accent) {
                gui_circle(win, hot.x + 17, hot.y + 17, 16, GUI_TEXT);
                gui_icon(win, hot.x + 9, hot.y + 9, 16, GUI_ICON_CHECK,
                         i == 4 ? 0x172033U : GUI_WHITE);
            }
            if (clicked(ui, hot))
                settings->accent = i;
        }
    }

    paint_preview(win, settings);
}

static void wallpaper_tile(gui_window *win, gui_ui *ui,
                           gui_settings *settings, int index, int x, int w,
                           const char *label)
{
    gui_rc r = gui_mkrc(x, 108, w, 106);
    uint32_t a = GUI_DESK_TOP, b = GUI_DESK_BOTTOM;

    if (index == 3) {
        a = gui_mix(a, 0xFB7185U, 90);
        b = gui_mix(b, 0x8B5CF6U, 70);
    }
    gui_round_rect(win, r, 8, GUI_PANEL);
    gui_vgradient(win, gui_mkrc(r.x + 6, r.y + 6, r.w - 12, 68), a, b);
    if (index == 0)
        gui_disc(win, r.x + r.w - 30, r.y + 30, 26,
                 gui_mix(a, GUI_ACCENT, 64));
    else if (index == 1)
        for (int i = 0; i < 4; i++)
            gui_line(win, r.x + 7, r.y + 30 + i * 11,
                     r.x + r.w - 7, r.y + 10 + i * 11, GUI_ACCENT);
    else if (index == 2)
        gui_round_frame(win, gui_mkrc(r.x + 20, r.y + 20, r.w - 40, 36),
                        8, gui_mix(a, GUI_TEXT, 80));
    else
        gui_disc(win, r.x + r.w / 2 + 12, r.y + 36, 26,
                 gui_mix(b, 0xFBBF24U, 70));
    gui_draw_text_aa(win, r.x + (r.w - gui_measure_text_aa(label, FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, 0)) / 2, r.y + 82,
                     label, FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                     GUI_TEXT, GUI_TRANSPARENT);
    gui_stroke_round_rect(win, r, 8, settings->wallpaper == index ? 2 : 1,
                          settings->wallpaper == index ? GUI_ACCENT
                                                       : GUI_EDGE);
    if (clicked(ui, r))
        settings->wallpaper = index;
}

static void paint_desktop(gui_window *win, gui_ui *ui,
                          gui_settings *settings)
{
    static const char *names[4] = { "Aurora", "Contour", "Quiet", "Sunset" };
    int content_x = NAV_W + 24;
    int content_w = win->w - NAV_W - 48;
    int gap = 10;
    int tile_w = (content_w - 3 * gap) / 4;
    int compact = settings->compact;

    section_title(win, 20, "Desktop & Workspaces",
                  "Select wallpaper, dock density, and Wind-Hover workspaces.");
    gui_draw_text_aa(win, content_x, 82, "Wallpaper", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    for (int i = 0; i < 4; i++) {
        wallpaper_tile(win, ui, settings, i, content_x + i * (tile_w + gap),
                       tile_w, names[i]);
    }

    gui_card(win, gui_mkrc(content_x, 234, content_w, 96));
    if (gui_toggle(win, content_x + 18, 256, "Use compact bottom dock",
                   &compact, ui))
        settings->compact = compact;
    gui_draw_text_aa(win, content_x + 18, 290,
                     "Compact mode reduces dock height to maximize workspace.",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_card(win, gui_mkrc(content_x, 346, content_w, 108));
    gui_draw_text_aa(win, content_x + 18, 362,
                     "Wind-Hover Workspaces & Hotkeys", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    gui_draw_text_aa(win, content_x + 18, 388,
                     "Active workspace count: 4  (Alt+1 .. Alt+4 to switch)",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_ACCENT, GUI_TRANSPARENT);
    gui_draw_text_aa(win, content_x + 18, 412,
                     "Super: Perch   Ctrl+T: Talon Shelf   Alt+Tab: Switch   Alt+F4: Close",
                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                     GUI_TEXT_DIM, GUI_TRANSPARENT);
}

static void value_row(gui_window *win, int y, const char *label,
                      const char *value)
{
    gui_draw_text_aa(win, NAV_W + 42, y, label, FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_text(win, NAV_W + 190, y - 2, value, GUI_TEXT, GUI_TRANSPARENT);
}

static void paint_system(gui_window *win, gui_ui *ui, int talon_n,
                         const struct k_parcel_info *slot0)
{
    struct k_cpuinfo cpu;
    struct k_netinfo net;
    struct k_fbinfo fb;
    struct k_psinfo ps;
    struct k_stat st;
    uint64_t total = 0, free_kb = 0, swap_total = 0, swap_used = 0;
    int content_x = NAV_W + 24;
    int content_w = win->w - NAV_W - 48;
    char buf[96], ip[16];
    int processes = 0, btn_y;

    section_title(win, 20, "System & Telemetry",
                  "Live hardware, workspaces, Talon clipboard and network.");
    {
        int card_h = win->h - 78 - 108;
        if (card_h < 292)
            card_h = 292;
        gui_card(win, gui_mkrc(content_x, 78, content_w, card_h));
        btn_y = 78 + card_h + 12;
    }

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    snprintf(buf, sizeof(buf), "%u online / %u discovered (cur CPU %u)",
             cpu.online, cpu.discovered, cpu.current_cpu);
    value_row(win, 98, "Processors", buf);

    if (meminfo(&total, &free_kb) == 0 && total) {
        uint64_t used = total - free_kb;
        int pct = (int)((used * 100) / total);

        snprintf(buf, sizeof(buf), "%llu / %llu MiB (%d%%)",
                 (unsigned long long)(used / 1024),
                 (unsigned long long)(total / 1024), pct);
        value_row(win, 128, "Memory", buf);
        gui_progress(win, gui_mkrc(NAV_W + 190, 150, content_w - 208, 10),
                     pct, pct > 85 ? GUI_ERROR : GUI_ACCENT);
    }

    swapinfo(&swap_total, &swap_used);
    if (swap_total > 0) {
        snprintf(buf, sizeof(buf), "%llu / %llu MiB",
                 (unsigned long long)(swap_used / 1024),
                 (unsigned long long)(swap_total / 1024));
    } else {
        snprintf(buf, sizeof(buf), "0 MiB (none configured)");
    }
    value_row(win, 174, "Swap", buf);

    snprintf(buf, sizeof(buf), "%d active (Wind-Hover 1..4)",
             K_WM_MAX_WORKSPACES);
    value_row(win, 204, "Workspaces", buf);

    if (talon_n > 0 && slot0 && slot0->label[0]) {
        snprintf(buf, sizeof(buf), "%d / %d active (top: %s)",
                 talon_n, K_PARCEL_MAX_SLOTS, slot0->label);
    } else {
        snprintf(buf, sizeof(buf), "%d / %d active",
                 talon_n, K_PARCEL_MAX_SLOTS);
    }
    value_row(win, 234, "Talon Parcels", buf);

    while (psinfo(processes, &ps) == 0)
        processes++;
    snprintf(buf, sizeof(buf), "%d running", processes);
    value_row(win, 264, "Processes", buf);

    memset(&fb, 0, sizeof(fb));
    syscall(SYS_FBINFO, (long)&fb, 0, 0, 0);
    snprintf(buf, sizeof(buf), "%ux%u at %u bpp", fb.width, fb.height, fb.bpp);
    value_row(win, 294, "Display", buf);

    if (netinfo(&net) == 0 && net.up)
        snprintf(buf, sizeof(buf), "%s", ip_ntoa(net.ip, ip));
    else
        snprintf(buf, sizeof(buf), "offline");
    value_row(win, 324, "Network", buf);

    snprintf(buf, sizeof(buf), "%luh %02lum %02lus",
             uptime_ms() / 3600000, (uptime_ms() / 60000) % 60,
             (uptime_ms() / 1000) % 60);
    value_row(win, 354, "Uptime", buf);

    if (btn_y + 34 > win->h - 56)
        btn_y = win->h - 90;
    if (gui_icon_button(win, gui_mkrc(content_x, btn_y, 176, 34),
                        GUI_ICON_MONITOR, "System Monitor", ui, 1, 0)) {
        char *argv[] = { "/bin/sysmon", 0 };
        spawn("/bin/sysmon", argv);
    }
    if (stat_("/bin/sight", &st) == 0 && !st.is_dir) {
        if (gui_icon_button(win, gui_mkrc(content_x + 188, btn_y, 128, 34),
                            GUI_ICON_SEARCH, "Sight", ui, 1, 0)) {
            char *argv[] = { "/bin/sight", 0 };
            spawn("/bin/sight", argv);
        }
        if (gui_icon_button(win, gui_mkrc(content_x + 328, btn_y, 124, 34),
                            GUI_ICON_INFO, "About", ui, 1, 0)) {
            char *argv[] = { "/bin/about", 0 };
            spawn("/bin/about", argv);
        }
    } else if (gui_icon_button(win, gui_mkrc(content_x + 188, btn_y, 134, 34),
                               GUI_ICON_INFO, "About", ui, 1, 0)) {
        char *argv[] = { "/bin/about", 0 };
        spawn("/bin/about", argv);
    }
}

static void paint(gui_window *win, gui_ui *ui, int *page,
                  gui_settings *settings, gui_settings *saved,
                  char status[96], int talon_n,
                  const struct k_parcel_info *slot0)
{
    int changed;
    int w = win->w, h = win->h;

    gui_clear(win, GUI_SURFACE);
    paint_nav(win, ui, page, talon_n);
    gui_push_clip(win, gui_mkrc(NAV_W, 0, w - NAV_W, h - 52));
    if (*page == PAGE_APPEARANCE)
        paint_appearance(win, ui, settings, saved, status);
    else if (*page == PAGE_DESKTOP)
        paint_desktop(win, ui, settings);
    else
        paint_system(win, ui, talon_n, slot0);
    gui_pop_clip(win);

    changed = memcmp(settings, saved, sizeof(*settings)) != 0;
    gui_rect(win, NAV_W, h - 52, w - NAV_W, 52, GUI_PANEL);
    gui_rect(win, NAV_W, h - 52, w - NAV_W, 1, GUI_EDGE);
    gui_push_clip(win, gui_mkrc(NAV_W + 14, h - 52, w - NAV_W - 220, 52));
    gui_draw_text_aa(win, NAV_W + 18, h - 32, status, FONT_SMALL,
                     FONT_REGULAR, FONT_ROMAN,
                     changed ? GUI_WARN : GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_pop_clip(win);
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    struct k_pulse_query pq;
    struct k_pulse pbuf[8];
    gui_settings settings, saved;
    struct k_parcel_info slot0;
    char status[96] = "Changes apply live across desktop & apps.";
    int page = PAGE_APPEARANCE, dirty = 1, talon_n;
    unsigned long next_system = 0;

    (void)argc;
    (void)argv;
    memset(&ui, 0, sizeof(ui));
    memset(&slot0, 0, sizeof(slot0));
    gui_settings_get(&settings);
    saved = settings;
    talon_n = count_talon_parcels(&slot0);

    memset(&pq, 0, sizeof(pq));
    pq.topic_mask = K_TOPIC_PARCEL | K_TOPIC_THEME;
    pq.max_out = 0;
    pq.timeout_ms = 0;
    (void)pulse_poll(&pq, 0);

    win = gui_open("Settings", 140, 80, DEF_W, DEF_H, 0);
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
            if (ev.type == KEV_THEME) {
                if (memcmp(&settings, &saved, sizeof(settings)) == 0) {
                    gui_settings_get(&settings);
                    saved = settings;
                }
            } else if (ev.type == KEV_KEY && ev.key == 27) {
                settings = saved;
                snprintf(status, sizeof(status), "Unsaved changes reverted.");
            }
            dirty = 1;
        }

        pq.topic_mask = K_TOPIC_PARCEL | K_TOPIC_THEME;
        pq.max_out = 8;
        pq.timeout_ms = 0;
        if (pulse_poll(&pq, pbuf) > 0) {
            talon_n = count_talon_parcels(&slot0);
            dirty = 1;
        }

        if (page == PAGE_SYSTEM && uptime_ms() >= next_system) {
            next_system = uptime_ms() + 1000;
            talon_n = count_talon_parcels(&slot0);
            dirty = 1;
        }
        if (!dirty)
            continue;

        paint(win, &ui, &page, &settings, &saved, status, talon_n, &slot0);
        if (page != PAGE_SYSTEM) {
            int w = win->w, h = win->h;
            int changed = memcmp(&settings, &saved, sizeof(settings)) != 0;

            if (gui_button_ex(win, gui_mkrc(w - 200, h - 42, 84, 32),
                              "Reset", &ui, changed, 0)) {
                settings = saved;
                snprintf(status, sizeof(status), "Unsaved changes reverted.");
            }
            if (gui_button_ex(win, gui_mkrc(w - 106, h - 42, 88, 32),
                              "Apply", &ui, changed, changed)) {
                if (settings.preset >= 0 && settings.preset < 4 &&
                    settings.preset != saved.preset &&
                    settings.accent == presets[settings.preset].default_accent)
                    gui_theme_set_preset(settings.preset);
                if (gui_settings_save(&settings) == 0) {
                    gui_settings_get(&settings);
                    saved = settings;
                    snprintf(status, sizeof(status),
                             "Appearance saved and broadcast live.");
                } else {
                    snprintf(status, sizeof(status),
                             "Could not write desktop preferences.");
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
