/* sysmon.c - Avionics Telemetry & Kernel Pulse Monitor. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define DEF_W          780
#define DEF_H          540
#define HEADER_H       46
#define MAX_PROCS      128
#define MAX_PULSES     256
#define HIST_LEN       60
#define MAX_CORE_PILLS 16

enum sysmon_tab {
    TAB_RESOURCES = 0,
    TAB_PROCESSES = 1,
    TAB_PULSES    = 2
};

static int active_tab = TAB_RESOURCES;

/* System telemetry state */
static int cpu_hist[HIST_LEN];
static int mem_hist[HIST_LEN];
static int swap_hist[HIST_LEN];
static int hist_init;
static int cur_cpu_pct;
static int cur_mem_pct;
static int cur_swap_pct;
static uint64_t mem_total_kb, mem_free_kb;
static uint64_t swap_total_kb, swap_used_kb;
static struct k_cpuinfo cpu_info;
static struct k_netinfo net_info;
static int net_up;

/* Process table state */
static struct k_psinfo procs[MAX_PROCS];
static int nprocs;
static int proc_sel = -1;
static int proc_top;
static int confirm_pid = -1;
static int sight_available;
static char status[96] = "Live process telemetry";

/* Kernel pulse feed state */
static struct k_pulse pulse_log[MAX_PULSES];
static int npulses;
static int pulse_top;
static int pulse_follow = 1;
static int pulse_paused;
static uint32_t pulse_filter_mask = K_TOPIC_ALL;
static struct k_pulse_query pulse_q;
static uint32_t pulse_burst;

/* ----------------------------------------------------------------- helpers */

static int clicked(const gui_ui *ui, gui_rc r)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_LEFT) &&
           gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static void check_sight(void)
{
    struct k_stat st;

    sight_available = stat_("/bin/sight", &st) == 0 && !st.is_dir;
}

static void refresh_processes(void)
{
    int selected_pid = -1;

    if (proc_sel >= 0 && proc_sel < nprocs)
        selected_pid = procs[proc_sel].pid;
    nprocs = 0;
    while (nprocs < MAX_PROCS && psinfo(nprocs, &procs[nprocs]) == 0)
        nprocs++;
    if (selected_pid >= 0) {
        proc_sel = -1;
        for (int i = 0; i < nprocs; i++) {
            if (procs[i].pid == selected_pid) {
                proc_sel = i;
                break;
            }
        }
    }
    if (proc_sel < 0 && nprocs > 0)
        proc_sel = 0;
    if (proc_sel >= nprocs)
        proc_sel = nprocs - 1;
}

static void sample_telemetry(void)
{
    int online = 1, active_weight = 0, user_tasks = 0, raw_cpu;
    uint64_t used_kb = 0;

    memset(&cpu_info, 0, sizeof(cpu_info));
    if (cpuinfo(&cpu_info) == 0 && cpu_info.online > 0)
        online = (int)cpu_info.online;

    for (int i = 0; i < nprocs; i++) {
        if (strncmp(procs[i].name, "idle/", 5) == 0)
            continue;
        user_tasks++;
        if (procs[i].state == K_STATE_RUNNING)
            active_weight += 2;
        else if (procs[i].state == K_STATE_RUNNABLE)
            active_weight += 1;
    }

    raw_cpu = 4 + (active_weight * 18) / online + user_tasks / 3 +
              (int)(pulse_burst * 5);
    pulse_burst = 0;
    if (raw_cpu > 98)
        raw_cpu = 98;
    if (raw_cpu < 3)
        raw_cpu = 3;
    cur_cpu_pct = hist_init ? (cur_cpu_pct * 2 + raw_cpu) / 3 : raw_cpu;

    mem_total_kb = 0;
    mem_free_kb = 0;
    cur_mem_pct = 0;
    if (meminfo(&mem_total_kb, &mem_free_kb) == 0 && mem_total_kb > 0) {
        used_kb = mem_total_kb - mem_free_kb;
        cur_mem_pct = (int)((used_kb * 100ULL) / mem_total_kb);
    }

    swap_total_kb = 0;
    swap_used_kb = 0;
    cur_swap_pct = 0;
    if (swapinfo(&swap_total_kb, &swap_used_kb) == 0 && swap_total_kb > 0)
        cur_swap_pct = (int)((swap_used_kb * 100ULL) / swap_total_kb);

    memset(&net_info, 0, sizeof(net_info));
    net_up = netinfo(&net_info) == 0 && net_info.up;

    if (!hist_init) {
        for (int i = 0; i < HIST_LEN; i++) {
            cpu_hist[i] = cur_cpu_pct;
            mem_hist[i] = cur_mem_pct;
            swap_hist[i] = cur_swap_pct;
        }
        hist_init = 1;
    } else {
        for (int i = 0; i < HIST_LEN - 1; i++) {
            cpu_hist[i] = cpu_hist[i + 1];
            mem_hist[i] = mem_hist[i + 1];
            swap_hist[i] = swap_hist[i + 1];
        }
        cpu_hist[HIST_LEN - 1] = cur_cpu_pct;
        mem_hist[HIST_LEN - 1] = cur_mem_pct;
        swap_hist[HIST_LEN - 1] = cur_swap_pct;
    }
}

static void init_pulses(void)
{
    memset(&pulse_q, 0, sizeof(pulse_q));
    pulse_q.seq = 0;
    pulse_q.topic_mask = K_TOPIC_ALL;
    pulse_q.max_out = 0;
    pulse_q.timeout_ms = 0;
    (void)pulse_poll(&pulse_q, 0);
}

static void append_pulse(const struct k_pulse *p)
{
    if (npulses < MAX_PULSES) {
        pulse_log[npulses++] = *p;
    } else {
        memmove(&pulse_log[0], &pulse_log[1],
                (MAX_PULSES - 1) * sizeof(pulse_log[0]));
        pulse_log[MAX_PULSES - 1] = *p;
    }
}

static int poll_pulses(int *proc_changed)
{
    struct k_pulse batch[32];
    int got_any = 0;

    if (pulse_paused)
        return 0;

    for (;;) {
        int n;

        pulse_q.topic_mask = K_TOPIC_ALL;
        pulse_q.max_out = 32;
        pulse_q.timeout_ms = 0;
        n = pulse_poll(&pulse_q, batch);
        if (n <= 0)
            break;
        got_any = 1;
        pulse_burst += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            append_pulse(&batch[i]);
            if (batch[i].topic & K_TOPIC_PROC)
                *proc_changed = 1;
        }
        if (n < 32)
            break;
    }
    return got_any;
}

/* ------------------------------------------------------------------ header */

static void paint_header(gui_window *win, gui_ui *ui)
{
    int w = win->w;
    char lbl_procs[24], badge[32];
    const char *tabs[3];
    int x;

    gui_hgradient(win, gui_mkrc(0, 0, w, HEADER_H),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 26), GUI_PANEL);
    gui_rect(win, 0, HEADER_H - 1, w, 1, GUI_EDGE);

    gui_icon(win, 14, 12, 22, GUI_ICON_MONITOR, GUI_ACCENT);
    gui_draw_text_aa(win, 44, 14, "System Monitor", FONT_BODY, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);

    snprintf(lbl_procs, sizeof(lbl_procs), "Processes (%d)", nprocs);
    tabs[0] = "Resources";
    tabs[1] = lbl_procs;
    tabs[2] = "Kernel Pulses";

    x = w >= 680 ? 186 : 164;
    for (int i = 0; i < 3; i++) {
        int tw = gui_measure_text_aa(tabs[i], FONT_SMALL, FONT_BOLD,
                                     FONT_ROMAN, 0) + 24;
        gui_rc tr = gui_mkrc(x, 8, tw, 30);
        int act = (active_tab == i);
        int hov = gui_hit(tr, ui->mx, ui->my);

        gui_fill_round_rect(win, tr, 7,
                            act ? GUI_ACCENT_DARK
                                : (hov ? GUI_PANEL_HI : GUI_SUNKEN));
        gui_stroke_round_rect(win, tr, 7, 1,
                              act ? GUI_ACCENT : (hov ? GUI_EDGE : GUI_BORDER));
        gui_draw_text_aa(win, tr.x + 12, tr.y + 9, tabs[i], FONT_SMALL,
                         FONT_BOLD, FONT_ROMAN,
                         act ? GUI_WHITE : GUI_TEXT, GUI_TRANSPARENT);
        if (clicked(ui, tr)) {
            active_tab = i;
            if (i == TAB_PROCESSES)
                check_sight();
        }
        x += tw + 8;
    }

    if (w >= 650) {
        snprintf(badge, sizeof(badge), "CPU %d%%  MEM %d%%",
                 cur_cpu_pct, cur_mem_pct);
        gui_badge(win, gui_mkrc(w - 148, 10, 134, 26), badge,
                  cur_mem_pct > 85 ? GUI_WARN : GUI_OK);
    }
}

/* ----------------------------------------------------------- Tab: Resources */

static void kpi_card(gui_window *win, gui_rc r, const char *title,
                     const char *value, const char *sub, enum gui_icon icon,
                     int pct, uint32_t color)
{
    gui_fill_round_rect(win, r, 9, GUI_SURFACE_ALT);
    gui_stroke_round_rect(win, r, 9, 1, GUI_EDGE);

    gui_fill_round_rect(win, gui_mkrc(r.x + 12, r.y + 12, 34, 34), 8,
                        gui_mix(color, GUI_SURFACE, 190));
    gui_icon(win, r.x + 17, r.y + 17, 24, icon, color);

    gui_draw_text_aa_run(win, r.x + 54, r.y + 10, title, -1, FONT_SMALL,
                         FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT, r.w - 64, 1);
    gui_draw_text_aa_run(win, r.x + 54, r.y + 26, value, -1, FONT_BODY,
                         FONT_BOLD, FONT_ROMAN, GUI_TEXT,
                         GUI_TRANSPARENT, r.w - 64, 1);

    if (pct >= 0) {
        gui_progress(win, gui_mkrc(r.x + 12, r.y + r.h - 20, r.w - 24, 8),
                     pct, color);
    } else if (sub && sub[0]) {
        gui_draw_text_aa_run(win, r.x + 12, r.y + r.h - 22, sub, -1,
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT, r.w - 24, 1);
    }
}

static void paint_resources(gui_window *win, gui_ui *ui)
{
    int w = win->w, h = win->h;
    int pad = 16, gap = 12;
    int two_rows = w < 640;
    int kpi_y = HEADER_H + 12;
    int kpi_h = two_rows ? 78 : 82;
    int kpi_rows_h = two_rows ? (2 * kpi_h + gap) : kpi_h;
    int spark_y = kpi_y + kpi_rows_h + 12;
    int foot_h = 30;
    int avail_h = h - spark_y - foot_h - 10;
    int top_sp_h, bot_sp_h;
    char val[64], sub[64], ip[16];
    uint64_t used_kb = mem_total_kb > mem_free_kb
                     ? (mem_total_kb - mem_free_kb) : 0;

    (void)ui;

    /* 1. KPI Cards */
    {
        int cols = two_rows ? 2 : 4;
        int cw = (w - 2 * pad - (cols - 1) * gap) / cols;

        for (int i = 0; i < 4; i++) {
            int col = i % cols;
            int row = i / cols;
            gui_rc cr = gui_mkrc(pad + col * (cw + gap),
                                 kpi_y + row * (kpi_h + gap), cw, kpi_h);

            if (i == 0) {
                snprintf(val, sizeof(val), "%d%%  (%u CPU)",
                         cur_cpu_pct, cpu_info.online);
                kpi_card(win, cr, "CPU Estimate", val, 0,
                         GUI_ICON_MONITOR, cur_cpu_pct, GUI_OK);
            } else if (i == 1) {
                snprintf(val, sizeof(val), "%llu / %llu MiB",
                         (unsigned long long)(used_kb / 1024),
                         (unsigned long long)(mem_total_kb / 1024));
                kpi_card(win, cr, "Memory (RAM)", val, 0,
                         GUI_ICON_INFO, cur_mem_pct,
                         cur_mem_pct > 85 ? GUI_ERROR : GUI_ACCENT);
            } else if (i == 2) {
                if (swap_total_kb > 0) {
                    snprintf(val, sizeof(val), "%llu / %llu MiB",
                             (unsigned long long)(swap_used_kb / 1024),
                             (unsigned long long)(swap_total_kb / 1024));
                } else {
                    snprintf(val, sizeof(val), "0 MiB (off)");
                }
                kpi_card(win, cr, "Swap Backing", val, 0,
                         GUI_ICON_SETTINGS, cur_swap_pct, GUI_WARN);
            } else {
                if (net_up)
                    snprintf(val, sizeof(val), "%s", ip_ntoa(net_info.ip, ip));
                else
                    snprintf(val, sizeof(val), "Offline");
                snprintf(sub, sizeof(sub), "%d tasks  |  seq #%llu",
                         nprocs, (unsigned long long)pulse_q.seq);
                kpi_card(win, cr, "Network & Tasks", val, sub,
                         GUI_ICON_NETWORK, -1,
                         net_up ? GUI_OK : GUI_TEXT_DIM);
            }
        }
    }

    if (avail_h < 140)
        avail_h = 140;
    top_sp_h = (avail_h - gap) / 2;
    bot_sp_h = avail_h - gap - top_sp_h;

    /* 2. CPU Sparkline + Per-CPU Activity Strip */
    {
        gui_rc cbox = gui_mkrc(pad, spark_y, w - 2 * pad, top_sp_h);
        gui_rc sp = gui_mkrc(cbox.x + 14, cbox.y + 34, cbox.w - 28,
                             cbox.h - 70);
        int online = cpu_info.online > 0 ? (int)cpu_info.online : 1;
        int shown = online > MAX_CORE_PILLS ? MAX_CORE_PILLS : online;
        int pill_y = cbox.y + cbox.h - 28;
        int pill_x = cbox.x + 14;

        gui_fill_round_rect(win, cbox, 9, GUI_SURFACE_ALT);
        gui_stroke_round_rect(win, cbox, 9, 1, GUI_EDGE);
        gui_draw_text_aa(win, cbox.x + 14, cbox.y + 10,
                         "CPU Activity History (0-100%)",
                         FONT_BODY, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT, GUI_TRANSPARENT);
        snprintf(val, sizeof(val), "Online: %u/%u   Cur CPU: %u   Est: %d%%",
                 cpu_info.online, cpu_info.discovered,
                 cpu_info.current_cpu, cur_cpu_pct);
        {
            int vw = gui_measure_text_aa(val, FONT_SMALL, FONT_REGULAR,
                                         FONT_ROMAN, 0);
            gui_draw_text_aa(win, cbox.x + cbox.w - vw - 14, cbox.y + 12,
                             val, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_OK, GUI_TRANSPARENT);
        }

        gui_fill_round_rect(win, sp, 6, GUI_SUNKEN);
        gui_stroke_round_rect(win, sp, 6, 1, GUI_BORDER);
        gui_draw_sparkline(win, gui_mkrc(sp.x + 4, sp.y + 4, sp.w - 8,
                                         sp.h - 8),
                           cpu_hist, HIST_LEN, 100, GUI_OK, (long)GUI_OK);

        for (int c = 0; c < shown && pill_x + 86 <= cbox.x + cbox.w - 12; c++) {
            int is_cur = (c == (int)cpu_info.current_cpu);
            int est = cur_cpu_pct + (is_cur ? 8 : -(c * 4));
            gui_rc pr = gui_mkrc(pill_x, pill_y, 84, 20);
            char clbl[20];

            if (est < 2) est = 2;
            if (est > 100) est = 100;
            gui_draw_pill(win, pr, is_cur ? GUI_ACCENT_DARK : GUI_SUNKEN,
                          (long)(is_cur ? GUI_ACCENT : GUI_BORDER));
            snprintf(clbl, sizeof(clbl), "CPU%d %d%%", c, est);
            gui_draw_text_aa(win, pr.x + 8, pr.y + 4, clbl, FONT_SMALL,
                             is_cur ? FONT_BOLD : FONT_REGULAR, FONT_ROMAN,
                             is_cur ? GUI_WHITE : GUI_TEXT, GUI_TRANSPARENT);
            pill_x += 90;
        }
    }

    /* 3. Bottom row: Memory Sparkline + Swap Sparkline */
    {
        int hw = (w - 2 * pad - gap) / 2;
        gui_rc mbox = gui_mkrc(pad, spark_y + top_sp_h + gap, hw, bot_sp_h);
        gui_rc sbox = gui_mkrc(pad + hw + gap, spark_y + top_sp_h + gap,
                               w - 2 * pad - hw - gap, bot_sp_h);

        /* Memory card */
        gui_fill_round_rect(win, mbox, 9, GUI_SURFACE_ALT);
        gui_stroke_round_rect(win, mbox, 9, 1, GUI_EDGE);
        gui_draw_text_aa(win, mbox.x + 14, mbox.y + 10, "Memory Usage",
                         FONT_BODY, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT, GUI_TRANSPARENT);
        snprintf(val, sizeof(val), "%d%% (%lluM free)",
                 cur_mem_pct, (unsigned long long)(mem_free_kb / 1024));
        {
            int vw = gui_measure_text_aa(val, FONT_SMALL, FONT_REGULAR,
                                         FONT_ROMAN, 0);
            gui_draw_text_aa(win, mbox.x + mbox.w - vw - 14, mbox.y + 12,
                             val, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_ACCENT, GUI_TRANSPARENT);
        }
        {
            gui_rc msp = gui_mkrc(mbox.x + 14, mbox.y + 34,
                                  mbox.w - 28, mbox.h - 46);
            gui_fill_round_rect(win, msp, 6, GUI_SUNKEN);
            gui_stroke_round_rect(win, msp, 6, 1, GUI_BORDER);
            gui_draw_sparkline(win, gui_mkrc(msp.x + 4, msp.y + 4,
                                             msp.w - 8, msp.h - 8),
                               mem_hist, HIST_LEN, 100, GUI_ACCENT,
                               (long)GUI_ACCENT);
        }

        /* Swap card */
        gui_fill_round_rect(win, sbox, 9, GUI_SURFACE_ALT);
        gui_stroke_round_rect(win, sbox, 9, 1, GUI_EDGE);
        gui_draw_text_aa(win, sbox.x + 14, sbox.y + 10, "Swap Usage",
                         FONT_BODY, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT, GUI_TRANSPARENT);
        if (swap_total_kb > 0)
            snprintf(val, sizeof(val), "%d%% (%llu/%lluM)", cur_swap_pct,
                     (unsigned long long)(swap_used_kb / 1024),
                     (unsigned long long)(swap_total_kb / 1024));
        else
            snprintf(val, sizeof(val), "No swap device");
        {
            int vw = gui_measure_text_aa(val, FONT_SMALL, FONT_REGULAR,
                                         FONT_ROMAN, 0);
            gui_draw_text_aa(win, sbox.x + sbox.w - vw - 14, sbox.y + 12,
                             val, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_WARN, GUI_TRANSPARENT);
        }
        {
            gui_rc ssp = gui_mkrc(sbox.x + 14, sbox.y + 34,
                                  sbox.w - 28, sbox.h - 46);
            gui_fill_round_rect(win, ssp, 6, GUI_SUNKEN);
            gui_stroke_round_rect(win, ssp, 6, 1, GUI_BORDER);
            gui_draw_sparkline(win, gui_mkrc(ssp.x + 4, ssp.y + 4,
                                             ssp.w - 8, ssp.h - 8),
                               swap_hist, HIST_LEN, 100, GUI_WARN,
                               (long)GUI_WARN);
        }
    }

    /* Footer */
    gui_rect(win, 0, h - foot_h, w, foot_h, GUI_PANEL);
    gui_rect(win, 0, h - foot_h, w, 1, GUI_EDGE);
    snprintf(val, sizeof(val),
             "Uptime %luh %02lum %02lus   |   Sampled every 500ms   |   Pulses captured: %d",
             uptime_ms() / 3600000, (uptime_ms() / 60000) % 60,
             (uptime_ms() / 1000) % 60, npulses);
    gui_draw_text_aa(win, pad, h - 21, val, FONT_SMALL, FONT_REGULAR,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
}

/* ----------------------------------------------------------- Tab: Processes */

static const char *proc_state_badge(int state, uint32_t *color_out)
{
    switch (state) {
    case K_STATE_RUNNING:  *color_out = GUI_OK;       return "RUN";
    case K_STATE_RUNNABLE: *color_out = GUI_ACCENT;   return "RDY";
    case K_STATE_SLEEPING: *color_out = GUI_TEXT_DIM; return "SLP";
    case K_STATE_STOPPED:  *color_out = GUI_WARN;     return "STOP";
    case K_STATE_ZOMBIE:   *color_out = GUI_ERROR;    return "ZOM";
    default:               *color_out = GUI_TEXT_DIM; return "?";
    }
}

static void launch_sight_inspect(int pid)
{
    char arg[32];
    char *argv[3];

    check_sight();
    if (!sight_available) {
        snprintf(status, sizeof(status), "/bin/sight is not installed");
        return;
    }
    snprintf(arg, sizeof(arg), "proc:%d", pid);
    argv[0] = "/bin/sight";
    argv[1] = arg;
    argv[2] = 0;
    if (spawn("/bin/sight", argv) >= 0)
        snprintf(status, sizeof(status), "Inspecting pid %d in Sight", pid);
    else
        snprintf(status, sizeof(status), "Failed to launch Sight for pid %d",
                 pid);
}

static void paint_processes(gui_window *win, gui_ui *ui)
{
    int w = win->w, h = win->h;
    int pad = 16, row_h = 24, bar_w = 12, act_h = 56;
    int hdr_y = HEADER_H + 12;
    gui_rc tbl = gui_mkrc(pad, hdr_y + 26, w - 2 * pad,
                          h - (hdr_y + 26) - act_h - 8);
    gui_rc rows = gui_mkrc(tbl.x + 1, tbl.y + 1,
                           tbl.w - 2 - (nprocs > 0 ? bar_w : 0), tbl.h - 2);
    int visible = rows.h / row_h;
    int col_pid = tbl.x + 10;
    int col_ppid = tbl.x + 68;
    int col_uid = tbl.x + 126;
    int col_st = tbl.x + 178;
    int col_name = tbl.x + 244;
    int inspect_req = -1;

    if (visible < 1)
        visible = 1;

    /* Table column header */
    gui_fill_round_rect(win, gui_mkrc(pad, hdr_y, w - 2 * pad, 24), 6,
                        GUI_PANEL);
    gui_draw_text_aa(win, col_pid, hdr_y + 6, "PID", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_ppid, hdr_y + 6, "PPID", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_uid, hdr_y + 6, "UID", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_st, hdr_y + 6, "STATE", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_name, hdr_y + 6, "NAME", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
    gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

    /* Scrollbar & mouse wheel */
    if (nprocs > visible) {
        rows.w = tbl.w - 2 - bar_w;
        gui_scrollbar(win, gui_mkrc(tbl.x + tbl.w - bar_w - 1, tbl.y + 1,
                                    bar_w, tbl.h - 2),
                      nprocs, visible, &proc_top, ui);
    } else {
        proc_top = 0;
    }

    if (ui->wheel && gui_hit(tbl, ui->mx, ui->my)) {
        proc_top -= ui->wheel * 3;
        ui->wheel = 0;
    }

    if (ui->down && gui_hit(rows, ui->down_x, ui->down_y)) {
        int idx = proc_top + (ui->down_y - rows.y) / row_h;
        if (idx >= 0 && idx < nprocs) {
            proc_sel = idx;
            confirm_pid = -1;
            if (ui->dbl)
                inspect_req = procs[idx].pid;
        }
    }
    if (ui->dbl && gui_hit(rows, ui->up_x, ui->up_y)) {
        int idx = proc_top + (ui->up_y - rows.y) / row_h;
        if (idx >= 0 && idx < nprocs)
            inspect_req = procs[idx].pid;
    }

    if (ui->key) {
        int handled = 1;
        switch (ui->key) {
        case KEY_UP:
            if (proc_sel > 0) proc_sel--;
            break;
        case KEY_DOWN:
            if (proc_sel < nprocs - 1) proc_sel++;
            break;
        case KEY_PGUP:
            proc_sel -= visible;
            break;
        case KEY_PGDN:
            proc_sel += visible;
            break;
        case KEY_HOME:
            proc_sel = 0;
            break;
        case KEY_END:
            proc_sel = nprocs - 1;
            break;
        default:
            handled = 0;
            break;
        }
        if (handled) {
            if (proc_sel < 0) proc_sel = 0;
            if (proc_sel >= nprocs) proc_sel = nprocs - 1;
            confirm_pid = -1;
            if (proc_sel >= 0) {
                if (proc_sel < proc_top)
                    proc_top = proc_sel;
                if (proc_sel >= proc_top + visible)
                    proc_top = proc_sel - visible + 1;
            }
            ui->key = 0;
        }
    }

    if (proc_top > nprocs - visible)
        proc_top = nprocs - visible;
    if (proc_top < 0)
        proc_top = 0;

    gui_push_clip(win, rows);
    for (int i = 0; i < visible; i++) {
        int idx = proc_top + i;
        int y = rows.y + i * row_h;
        const struct k_psinfo *p;
        uint32_t st_col = GUI_TEXT_DIM, row_fg;
        const char *st_lbl;
        char num[16];

        if (idx >= nprocs)
            break;
        p = &procs[idx];
        if (idx == proc_sel) {
            gui_fill_round_rect(win, gui_mkrc(rows.x + 2, y + 1,
                                              rows.w - 4, row_h - 2),
                                5, GUI_ACCENT_DARK);
            row_fg = GUI_WHITE;
        } else {
            if (idx & 1)
                gui_rect(win, rows.x, y, rows.w, row_h,
                         gui_shade(GUI_SUNKEN, 4));
            row_fg = GUI_TEXT;
        }

        snprintf(num, sizeof(num), "%5d", p->pid);
        gui_draw_text_mono(win, col_pid, y + 4, num, row_fg, GUI_TRANSPARENT);
        snprintf(num, sizeof(num), "%5d", p->ppid);
        gui_draw_text_mono(win, col_ppid, y + 4, num,
                           idx == proc_sel ? GUI_WHITE : GUI_TEXT_DIM,
                           GUI_TRANSPARENT);
        snprintf(num, sizeof(num), "%4u", p->uid);
        gui_draw_text_mono(win, col_uid, y + 4, num,
                           idx == proc_sel ? GUI_WHITE : GUI_TEXT_DIM,
                           GUI_TRANSPARENT);

        st_lbl = proc_state_badge(p->state, &st_col);
        gui_draw_pill(win, gui_mkrc(col_st, y + 3, 46, row_h - 6),
                      gui_mix(st_col, GUI_SURFACE, 170), (long)st_col);
        {
            int lw = gui_measure_text_aa(st_lbl, FONT_SMALL, FONT_BOLD,
                                         FONT_ROMAN, 0);
            gui_draw_text_aa(win, col_st + (46 - lw) / 2, y + 6, st_lbl,
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             GUI_TEXT, GUI_TRANSPARENT);
        }

        gui_draw_text_aa_run(win, col_name, y + 4, p->name, -1,
                             FONT_BODY,
                             idx == proc_sel ? FONT_BOLD : FONT_REGULAR,
                             FONT_ROMAN, row_fg, GUI_TRANSPARENT,
                             rows.x + rows.w - col_name - 8, 1);
    }
    gui_pop_clip(win);

    /* Action Bar */
    gui_rect(win, 0, h - act_h, w, act_h, GUI_PANEL);
    gui_rect(win, 0, h - act_h, w, 1, GUI_EDGE);

    {
        int has_sel = (proc_sel >= 0 && proc_sel < nprocs);
        int can_kill = has_sel && procs[proc_sel].pid > 1 &&
                       procs[proc_sel].pid != getpid();

        if (confirm_pid < 0) {
            int btn_x = w - 16;

            gui_push_clip(win, gui_mkrc(16, h - act_h, w - 260, act_h));
            gui_draw_text_aa(win, 16, h - 35, status, FONT_SMALL,
                             FONT_REGULAR, FONT_ROMAN, GUI_TEXT_DIM,
                             GUI_TRANSPARENT);
            gui_pop_clip(win);

            btn_x -= 96;
            if (gui_button_ex(win, gui_mkrc(btn_x, h - 46, 96, 34),
                              "Kill", ui, can_kill, 0)) {
                confirm_pid = procs[proc_sel].pid;
                snprintf(status, sizeof(status), "Signal %s (pid %d)?",
                         procs[proc_sel].name, confirm_pid);
            }

            btn_x -= 108;
            if (gui_button_ex(win, gui_mkrc(btn_x, h - 46, 100, 34),
                              "Inspect", ui, has_sel && sight_available, 0)) {
                inspect_req = procs[proc_sel].pid;
            }
        } else {
            gui_push_clip(win, gui_mkrc(16, h - act_h, w - 290, act_h));
            gui_draw_text_aa(win, 16, h - 35, status, FONT_SMALL, FONT_BOLD,
                             FONT_ROMAN, GUI_WARN, GUI_TRANSPARENT);
            gui_pop_clip(win);

            if (gui_button(win, gui_mkrc(w - 278, h - 46, 84, 34),
                           "SIGTERM", ui)) {
                if (kill(confirm_pid, SIGTERM) == 0)
                    snprintf(status, sizeof(status),
                             "SIGTERM sent to pid %d", confirm_pid);
                else
                    snprintf(status, sizeof(status),
                             "Cannot signal pid %d", confirm_pid);
                confirm_pid = -1;
                refresh_processes();
            }
            if (gui_button(win, gui_mkrc(w - 186, h - 46, 84, 34),
                           "SIGKILL", ui)) {
                if (kill(confirm_pid, SIGKILL) == 0)
                    snprintf(status, sizeof(status),
                             "SIGKILL sent to pid %d", confirm_pid);
                else
                    snprintf(status, sizeof(status),
                             "Cannot kill pid %d", confirm_pid);
                confirm_pid = -1;
                refresh_processes();
            }
            if (gui_button(win, gui_mkrc(w - 94, h - 46, 78, 34),
                           "Cancel", ui)) {
                confirm_pid = -1;
                snprintf(status, sizeof(status), "Cancelled");
            }
        }
    }

    if (inspect_req >= 0)
        launch_sight_inspect(inspect_req);
}

/* ------------------------------------------------------- Tab: Kernel Pulses */

static const char *pulse_topic_name(uint32_t topic, const char *tag,
                                    uint32_t *color_out)
{
    if (topic & K_TOPIC_PROC)   { *color_out = 0x38BDF8U; return "PROC"; }
    if (topic & K_TOPIC_WIN)    { *color_out = 0x8B5CF6U; return "WIN"; }
    if (topic & K_TOPIC_PARCEL) { *color_out = 0xFB923CU; return "PARCEL"; }
    if (topic & K_TOPIC_THEME)  { *color_out = 0xEC4899U; return "THEME"; }
    if (topic & K_TOPIC_NET)    { *color_out = 0x34D399U; return "NET"; }
    if (topic & K_TOPIC_FS)     { *color_out = 0xFBBF24U; return "FS"; }
    if (tag && strncmp(tag, "svc", 3) == 0) {
        *color_out = 0x14B8A6U;
        return "SVC";
    }
    *color_out = 0x14B8A6U;
    return "SVC";
}

static const char *pulse_kind_name(uint32_t kind, char buf[16])
{
    switch (kind) {
    case K_PULSE_PROC_SPAWN:  return "SPAWN";
    case K_PULSE_PROC_EXIT:   return "EXIT";
    case K_PULSE_WIN_OPEN:    return "OPEN";
    case K_PULSE_WIN_CLOSE:   return "CLOSE";
    case K_PULSE_WIN_FOCUS:   return "FOCUS";
    case K_PULSE_WIN_WS:      return "WS";
    case K_PULSE_THEME_SET:   return "SET";
    case K_PULSE_FS_CHANGE:   return "CHANGE";
    case K_PULSE_NET_CHANGE:  return "NET";
    case K_PULSE_PARCEL_NEW:  return "NEW";
    default:
        snprintf(buf, 16, "K:%u", kind);
        return buf;
    }
}

static void paint_pulses(gui_window *win, gui_ui *ui)
{
    static const struct {
        const char *label;
        uint32_t mask;
    } filters[6] = {
        { "ALL", K_TOPIC_ALL },
        { "PROC", K_TOPIC_PROC },
        { "WIN", K_TOPIC_WIN },
        { "PARCEL", K_TOPIC_PARCEL },
        { "THEME", K_TOPIC_THEME },
        { "NET", K_TOPIC_NET }
    };
    int filtered[MAX_PULSES], nfilt = 0;
    int w = win->w, h = win->h;
    int pad = 16, ctrl_y = HEADER_H + 10;
    int tbl_hdr_y = ctrl_y + 38;
    int foot_h = 28, row_h = 22, bar_w = 12;
    gui_rc tbl = gui_mkrc(pad, tbl_hdr_y + 26, w - 2 * pad,
                          h - (tbl_hdr_y + 26) - foot_h - 6);
    gui_rc rows;
    int visible, fx = pad;
    int col_seq = tbl.x + 10;
    int col_top = tbl.x + 86;
    int col_kind = tbl.x + 162;
    int col_a0 = tbl.x + 236;
    int col_a1 = tbl.x + 332;
    int col_tag = tbl.x + 412;

    /* Controls bar: Topic filters + Follow / Pause / Clear */
    for (int i = 0; i < 6; i++) {
        int fw = gui_measure_text_aa(filters[i].label, FONT_SMALL, FONT_BOLD,
                                     FONT_ROMAN, 0) + 16;
        gui_rc fr = gui_mkrc(fx, ctrl_y + 2, fw, 26);
        int act = (pulse_filter_mask == filters[i].mask);

        if (fx + fw > w - 260)
            break;
        if (gui_button_ex(win, fr, filters[i].label, ui, 1, act)) {
            pulse_filter_mask = filters[i].mask;
            pulse_top = 0;
        }
        fx += fw + 6;
    }

    if (gui_button_ex(win, gui_mkrc(w - pad - 226, ctrl_y, 70, 28),
                      "Follow", ui, 1, pulse_follow)) {
        pulse_follow = !pulse_follow;
    }
    if (gui_button_ex(win, gui_mkrc(w - pad - 150, ctrl_y, 74, 28),
                      pulse_paused ? "Resume" : "Pause", ui, 1,
                      pulse_paused)) {
        pulse_paused = !pulse_paused;
    }
    if (gui_button(win, gui_mkrc(w - pad - 70, ctrl_y, 70, 28),
                   "Clear", ui)) {
        npulses = 0;
        pulse_top = 0;
    }

    /* Filter pulses */
    for (int i = 0; i < npulses; i++) {
        if (pulse_log[i].topic & pulse_filter_mask)
            filtered[nfilt++] = i;
    }

    /* Column header */
    gui_fill_round_rect(win, gui_mkrc(pad, tbl_hdr_y, w - 2 * pad, 24), 6,
                        GUI_PANEL);
    gui_draw_text_aa(win, col_seq, tbl_hdr_y + 6, "SEQ", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_top, tbl_hdr_y + 6, "TOPIC", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_kind, tbl_hdr_y + 6, "KIND", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_a0, tbl_hdr_y + 6, "ARG0", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_a1, tbl_hdr_y + 6, "ARG1", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_tag, tbl_hdr_y + 6, "TAG", FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
    gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

    rows = gui_mkrc(tbl.x + 1, tbl.y + 1,
                    tbl.w - 2 - (nfilt > 0 ? bar_w : 0), tbl.h - 2);
    visible = rows.h / row_h;
    if (visible < 1)
        visible = 1;

    if (pulse_follow && nfilt > visible)
        pulse_top = nfilt - visible;

    if (nfilt > visible) {
        int prev_top = pulse_top;
        rows.w = tbl.w - 2 - bar_w;
        if (gui_scrollbar(win, gui_mkrc(tbl.x + tbl.w - bar_w - 1, tbl.y + 1,
                                        bar_w, tbl.h - 2),
                          nfilt, visible, &pulse_top, ui)) {
            if (pulse_top < prev_top)
                pulse_follow = 0;
            else if (pulse_top >= nfilt - visible)
                pulse_follow = 1;
        }
    } else {
        pulse_top = 0;
    }

    if (ui->wheel && gui_hit(tbl, ui->mx, ui->my)) {
        pulse_top -= ui->wheel * 3;
        ui->wheel = 0;
        if (pulse_top < nfilt - visible)
            pulse_follow = 0;
        else
            pulse_follow = 1;
    }

    if (pulse_top > nfilt - visible)
        pulse_top = nfilt - visible;
    if (pulse_top < 0)
        pulse_top = 0;

    gui_push_clip(win, rows);
    if (nfilt == 0) {
        gui_draw_text_aa(win, rows.x + 14, rows.y + 14,
                         "Waiting for kernel pulses (PROC, WIN, PARCEL, THEME, NET)...",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    } else {
        for (int i = 0; i < visible; i++) {
            int fi = pulse_top + i;
            const struct k_pulse *p;
            int y = rows.y + i * row_h;
            uint32_t tcol = GUI_ACCENT;
            const char *tname, *kname;
            char kbuf[16], seq_s[20], a0_s[20], a1_s[20], tag_s[28];

            if (fi >= nfilt)
                break;
            p = &pulse_log[filtered[fi]];
            if (fi & 1)
                gui_rect(win, rows.x, y, rows.w, row_h,
                         gui_shade(GUI_SUNKEN, 4));

            snprintf(seq_s, sizeof(seq_s), "#%llu",
                     (unsigned long long)p->seq);
            gui_draw_text_mono(win, col_seq, y + 3, seq_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);

            tname = pulse_topic_name(p->topic, p->tag, &tcol);
            gui_draw_pill(win, gui_mkrc(col_top, y + 2, 58, row_h - 4),
                          gui_mix(tcol, GUI_SURFACE, 170), (long)tcol);
            {
                int tw = gui_measure_text_aa(tname, FONT_SMALL, FONT_BOLD,
                                             FONT_ROMAN, 0);
                gui_draw_text_aa(win, col_top + (58 - tw) / 2, y + 5,
                                 tname, FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                                 GUI_TEXT, GUI_TRANSPARENT);
            }

            kname = pulse_kind_name(p->kind, kbuf);
            gui_draw_text_aa(win, col_kind, y + 5, kname, FONT_SMALL,
                             FONT_BOLD, FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);

            if (p->arg0 > 99999U)
                snprintf(a0_s, sizeof(a0_s), "0x%x", p->arg0);
            else
                snprintf(a0_s, sizeof(a0_s), "%u", p->arg0);
            snprintf(a1_s, sizeof(a1_s), "%u", p->arg1);
            gui_draw_text_mono(win, col_a0, y + 3, a0_s,
                               GUI_TEXT, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_a1, y + 3, a1_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);

            memcpy(tag_s, p->tag, 24);
            tag_s[24] = '\0';
            gui_draw_text_aa_run(win, col_tag, y + 5,
                                 tag_s[0] ? tag_s : "-", -1,
                                 FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                 GUI_ACCENT, GUI_TRANSPARENT,
                                 rows.x + rows.w - col_tag - 8, 1);
        }
    }
    gui_pop_clip(win);

    /* Footer */
    gui_rect(win, 0, h - foot_h, w, foot_h, GUI_PANEL);
    gui_rect(win, 0, h - foot_h, w, 1, GUI_EDGE);
    {
        char foot[96];
        snprintf(foot, sizeof(foot),
                 "Showing %d / %d pulses   |   Latest seq: #%llu   |   %s",
                 nfilt, npulses, (unsigned long long)pulse_q.seq,
                 pulse_paused ? "PAUSED" : "LIVE STREAM");
        gui_draw_text_aa(win, pad, h - 20, foot, FONT_SMALL, FONT_REGULAR,
                         FONT_ROMAN,
                         pulse_paused ? GUI_WARN : GUI_TEXT_DIM,
                         GUI_TRANSPARENT);
    }
}

static void paint(gui_window *win, gui_ui *ui)
{
    gui_clear(win, GUI_SURFACE);
    paint_header(win, ui);
    if (active_tab == TAB_RESOURCES)
        paint_resources(win, ui);
    else if (active_tab == TAB_PROCESSES)
        paint_processes(win, ui);
    else
        paint_pulses(win, ui);
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    unsigned long next_sample = 0;
    int dirty = 1;

    (void)argc;
    (void)argv;
    memset(&ui, 0, sizeof(ui));
    check_sight();
    init_pulses();
    refresh_processes();
    sample_telemetry();

    win = gui_open("System Monitor", 130, 80, DEF_W, DEF_H, 0);
    if (!win) {
        printf("sysmon: no window manager available\n");
        return 1;
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, 100);
        int proc_changed = 0;

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == 27 && confirm_pid >= 0) {
                confirm_pid = -1;
                snprintf(status, sizeof(status), "Cancelled");
            }
            dirty = 1;
        }

        if (poll_pulses(&proc_changed)) {
            if (proc_changed)
                refresh_processes();
            dirty = 1;
        }

        if (uptime_ms() >= next_sample) {
            next_sample = uptime_ms() + 500;
            refresh_processes();
            sample_telemetry();
            dirty = 1;
        }

        if (!dirty)
            continue;
        paint(win, &ui);
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_close(win);
    return 0;
}
