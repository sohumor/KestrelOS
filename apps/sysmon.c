/* sysmon.c - Avionics Telemetry & Kernel Pulse Monitor. */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define DEF_W          820
#define DEF_H          560
#define HEADER_H       46
#define MAX_PROCS      128
#define MAX_PULSES     256
#define HIST_LEN       60
#define MAX_CORE_PILLS 16

#define FD_MAX_EVENTS  256
#define FD_MAX_SOCKETS 32
#define FD_MAX_CAPS    128
#define FD_MAX_PARCELS 64
#define FD_NUM_CATS    6

enum sysmon_tab {
    TAB_RESOURCES = 0,
    TAB_PROCESSES = 1,
    TAB_PULSES    = 2,
    TAB_FLIGHT    = 3
};

enum flight_subview {
    FD_VIEW_EVENTS       = 0,
    FD_VIEW_SOCKETS      = 1,
    FD_VIEW_CAPS_PARCELS = 2
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

/* Flight Deck state (/dev/{flight,sockets,caps,parcels} @krec views) */
struct fd_event_row {
    uint64_t tick;
    int cpu;
    int pid;
    char cat[12];
    char event[20];
    char arg0[24];
    char detail[32];
};

struct fd_socket_row {
    int slot;
    int pid;
    int local_port;
    int peer_port;
    int rx_bytes;
    int tx_bytes;
    char state[16];
    char peer_ip[20];
};

struct fd_cap_row {
    int pid;
    uint32_t uid;
    char cap_mask[20];
    char caps[64];
    char unveil[64];
};

struct fd_parcel_row {
    int id;
    int uid;
    int pid;
    int size;
    char type[12];
    char mime[24];
    char label[48];
};

static const char *const fd_cat_names[FD_NUM_CATS] = {
    "proc", "fs", "vm", "pulse", "talon", "net"
};

static int flight_subview = FD_VIEW_EVENTS;
static int flight_cat_filter = -1;
static int flight_paused;
static int flight_follow = 1;

static struct fd_event_row fd_events[FD_MAX_EVENTS];
static int nfd_events;
static int fd_ev_top;
static int fd_ev_filtered[FD_MAX_EVENTS];

static struct fd_socket_row fd_sockets[FD_MAX_SOCKETS];
static int nfd_sockets;
static int fd_sock_top;

static struct fd_cap_row fd_caps[FD_MAX_CAPS];
static int nfd_caps;
static int fd_caps_top;

static struct fd_parcel_row fd_parcels[FD_MAX_PARCELS];
static int nfd_parcels;
static int fd_parcels_top;

static struct krec_reader fd_kr;
static struct krec_schema fd_ks;
static char fd_kline[512];
static char *fd_kfields[KREC_MAX_COLS];

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

static void fd_copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", (src && src[0]) ? src : "-");
}

static int fd_open_krec(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    krec_reader_init(&fd_kr, fd);
    if (krec_read_header(&fd_kr, &fd_ks) <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void refresh_flight_events(void)
{
    int fd = fd_open_krec("/dev/flight");
    if (fd < 0)
        return;

    int c_tick = krec_find_col(&fd_ks, "tick");
    int c_cpu = krec_find_col(&fd_ks, "cpu");
    int c_pid = krec_find_col(&fd_ks, "pid");
    int c_cat = krec_find_col(&fd_ks, "cat");
    int c_event = krec_find_col(&fd_ks, "event");
    int c_arg0 = krec_find_col(&fd_ks, "arg0");
    int c_detail = krec_find_col(&fd_ks, "detail");
    if (c_tick < 0 || c_cpu < 0 || c_pid < 0 || c_cat < 0 ||
        c_event < 0 || c_arg0 < 0 || c_detail < 0) {
        close(fd);
        return;
    }

    int count = 0;
    while (count < FD_MAX_EVENTS &&
           krec_next(&fd_kr, fd_kline, sizeof(fd_kline), fd_kfields,
                     KREC_MAX_COLS) == 1) {
        uint64_t tick = 0;
        int64_t cpu = 0, pid = 0;
        if (krec_parse_u64(fd_kfields[c_tick], &tick) < 0 ||
            krec_parse_i64(fd_kfields[c_cpu], &cpu) < 0 ||
            krec_parse_i64(fd_kfields[c_pid], &pid) < 0)
            continue;
        struct fd_event_row *r = &fd_events[count++];
        r->tick = tick;
        r->cpu = (int)cpu;
        r->pid = (int)pid;
        fd_copy_str(r->cat, sizeof(r->cat), fd_kfields[c_cat]);
        fd_copy_str(r->event, sizeof(r->event), fd_kfields[c_event]);
        fd_copy_str(r->arg0, sizeof(r->arg0), fd_kfields[c_arg0]);
        fd_copy_str(r->detail, sizeof(r->detail), fd_kfields[c_detail]);
    }
    close(fd);
    nfd_events = count;
}

static void refresh_flight_sockets(void)
{
    int fd = fd_open_krec("/dev/sockets");
    if (fd < 0)
        return;

    int c_slot = krec_find_col(&fd_ks, "slot");
    int c_pid = krec_find_col(&fd_ks, "pid");
    int c_state = krec_find_col(&fd_ks, "state");
    int c_lport = krec_find_col(&fd_ks, "local_port");
    int c_pip = krec_find_col(&fd_ks, "peer_ip");
    int c_pport = krec_find_col(&fd_ks, "peer_port");
    int c_rx = krec_find_col(&fd_ks, "rx_bytes");
    int c_tx = krec_find_col(&fd_ks, "tx_bytes");
    if (c_slot < 0 || c_pid < 0 || c_state < 0 || c_lport < 0 ||
        c_pip < 0 || c_pport < 0 || c_rx < 0 || c_tx < 0) {
        close(fd);
        return;
    }

    int count = 0;
    while (count < FD_MAX_SOCKETS &&
           krec_next(&fd_kr, fd_kline, sizeof(fd_kline), fd_kfields,
                     KREC_MAX_COLS) == 1) {
        int64_t slot = 0, pid = 0, lp = 0, pp = 0, rx = 0, tx = 0;
        if (krec_parse_i64(fd_kfields[c_slot], &slot) < 0 ||
            krec_parse_i64(fd_kfields[c_pid], &pid) < 0 ||
            krec_parse_i64(fd_kfields[c_lport], &lp) < 0 ||
            krec_parse_i64(fd_kfields[c_pport], &pp) < 0 ||
            krec_parse_i64(fd_kfields[c_rx], &rx) < 0 ||
            krec_parse_i64(fd_kfields[c_tx], &tx) < 0)
            continue;
        struct fd_socket_row *r = &fd_sockets[count++];
        r->slot = (int)slot;
        r->pid = (int)pid;
        r->local_port = (int)lp;
        r->peer_port = (int)pp;
        r->rx_bytes = (int)rx;
        r->tx_bytes = (int)tx;
        fd_copy_str(r->state, sizeof(r->state), fd_kfields[c_state]);
        fd_copy_str(r->peer_ip, sizeof(r->peer_ip), fd_kfields[c_pip]);
    }
    close(fd);
    nfd_sockets = count;
}

static void refresh_flight_caps(void)
{
    int fd = fd_open_krec("/dev/caps");
    if (fd < 0)
        return;

    int c_pid = krec_find_col(&fd_ks, "pid");
    int c_uid = krec_find_col(&fd_ks, "uid");
    int c_mask = krec_find_col(&fd_ks, "cap_mask");
    int c_caps = krec_find_col(&fd_ks, "caps");
    int c_unveil = krec_find_col(&fd_ks, "unveil");
    if (c_pid < 0 || c_uid < 0 || c_mask < 0 || c_caps < 0 || c_unveil < 0) {
        close(fd);
        return;
    }

    int count = 0;
    while (count < FD_MAX_CAPS &&
           krec_next(&fd_kr, fd_kline, sizeof(fd_kline), fd_kfields,
                     KREC_MAX_COLS) == 1) {
        int64_t pid = 0, uid = 0;
        if (krec_parse_i64(fd_kfields[c_pid], &pid) < 0 ||
            krec_parse_i64(fd_kfields[c_uid], &uid) < 0)
            continue;
        struct fd_cap_row *r = &fd_caps[count++];
        r->pid = (int)pid;
        r->uid = (uint32_t)uid;
        fd_copy_str(r->cap_mask, sizeof(r->cap_mask), fd_kfields[c_mask]);
        fd_copy_str(r->caps, sizeof(r->caps), fd_kfields[c_caps]);
        fd_copy_str(r->unveil, sizeof(r->unveil), fd_kfields[c_unveil]);
    }
    close(fd);
    nfd_caps = count;
}

static void refresh_flight_parcels(void)
{
    int fd = fd_open_krec("/dev/parcels");
    if (fd < 0)
        return;

    int c_id = krec_find_col(&fd_ks, "id");
    int c_uid = krec_find_col(&fd_ks, "uid");
    int c_pid = krec_find_col(&fd_ks, "pid");
    int c_type = krec_find_col(&fd_ks, "type");
    int c_size = krec_find_col(&fd_ks, "size");
    int c_mime = krec_find_col(&fd_ks, "mime");
    int c_label = krec_find_col(&fd_ks, "label");
    if (c_id < 0 || c_uid < 0 || c_pid < 0 || c_type < 0 ||
        c_size < 0 || c_mime < 0 || c_label < 0) {
        close(fd);
        return;
    }

    int count = 0;
    while (count < FD_MAX_PARCELS &&
           krec_next(&fd_kr, fd_kline, sizeof(fd_kline), fd_kfields,
                     KREC_MAX_COLS) == 1) {
        int64_t id = 0, uid = 0, pid = 0, sz = 0;
        if (krec_parse_i64(fd_kfields[c_id], &id) < 0 ||
            krec_parse_i64(fd_kfields[c_uid], &uid) < 0 ||
            krec_parse_i64(fd_kfields[c_pid], &pid) < 0 ||
            krec_parse_i64(fd_kfields[c_size], &sz) < 0)
            continue;
        struct fd_parcel_row *r = &fd_parcels[count++];
        r->id = (int)id;
        r->uid = (int)uid;
        r->pid = (int)pid;
        r->size = (int)sz;
        fd_copy_str(r->type, sizeof(r->type), fd_kfields[c_type]);
        fd_copy_str(r->mime, sizeof(r->mime), fd_kfields[c_mime]);
        fd_copy_str(r->label, sizeof(r->label), fd_kfields[c_label]);
    }
    close(fd);
    nfd_parcels = count;
}

static void refresh_flight_deck(int force)
{
    if (flight_paused && !force)
        return;
    refresh_flight_events();
    refresh_flight_sockets();
    refresh_flight_caps();
    refresh_flight_parcels();
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
    const char *tabs[4];
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
    tabs[3] = "Flight Deck";

    x = w >= 680 ? 174 : 158;
    for (int i = 0; i < 4; i++) {
        int tw = gui_measure_text_aa(tabs[i], FONT_SMALL, FONT_BOLD,
                                     FONT_ROMAN, 0) + 20;
        gui_rc tr = gui_mkrc(x, 8, tw, 30);
        int act = (active_tab == i);
        int hov = gui_hit(tr, ui->mx, ui->my);

        if (x + tw > w - 12)
            break;
        gui_fill_round_rect(win, tr, 7,
                            act ? GUI_ACCENT_DARK
                                : (hov ? GUI_PANEL_HI : GUI_SUNKEN));
        gui_stroke_round_rect(win, tr, 7, 1,
                              act ? GUI_ACCENT : (hov ? GUI_EDGE : GUI_BORDER));
        gui_draw_text_aa(win, tr.x + 10, tr.y + 9, tabs[i], FONT_SMALL,
                         FONT_BOLD, FONT_ROMAN,
                         act ? GUI_WHITE : GUI_TEXT, GUI_TRANSPARENT);
        if (clicked(ui, tr)) {
            active_tab = i;
            if (i == TAB_PROCESSES)
                check_sight();
            else if (i == TAB_FLIGHT)
                refresh_flight_deck(1);
        }
        x += tw + 6;
    }

    if (x + 142 <= w) {
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
    char val[96], sub[64], ip[16];
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

/* --------------------------------------------------------- Tab: Flight Deck */

static uint32_t flight_cat_color(const char *cat)
{
    if (strcmp(cat, "proc") == 0)  return 0x38BDF8U;
    if (strcmp(cat, "fs") == 0)    return 0xFBBF24U;
    if (strcmp(cat, "vm") == 0)    return 0x8B5CF6U;
    if (strcmp(cat, "pulse") == 0) return 0x14B8A6U;
    if (strcmp(cat, "talon") == 0) return 0xFB923CU;
    if (strcmp(cat, "net") == 0)   return 0x34D399U;
    return GUI_TEXT_DIM;
}

static uint32_t socket_state_color(const char *st)
{
    if (strcmp(st, "ESTABLISHED") == 0) return GUI_OK;
    if (strcmp(st, "SYN_SENT") == 0)    return GUI_ACCENT;
    if (strcmp(st, "CLOSED") == 0)      return GUI_TEXT_DIM;
    return GUI_WARN;
}

static uint32_t parcel_type_color(const char *type)
{
    if (strcmp(type, "url") == 0)    return 0x3B82F6U;
    if (strcmp(type, "color") == 0)  return 0xEC4899U;
    if (strcmp(type, "krec") == 0)   return 0x8B5CF6U;
    if (strcmp(type, "path") == 0)   return 0xF59E0BU;
    if (strcmp(type, "number") == 0) return 0x14B8A6U;
    if (strcmp(type, "image") == 0)  return 0x64748BU;
    return 0x38BDF8U;
}

static void fd_draw_pill_cell(gui_window *win, int x, int y, int pw, int ph,
                              const char *label, uint32_t col)
{
    int lw = gui_measure_text_aa(label, FONT_SMALL, FONT_BOLD, FONT_ROMAN, 0);

    gui_draw_pill(win, gui_mkrc(x, y, pw, ph),
                  gui_mix(col, GUI_SURFACE, 170), (long)col);
    gui_draw_text_aa(win, x + (pw - lw) / 2, y + 3, label, FONT_SMALL,
                     FONT_BOLD, FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
}

static void fd_scroll_handle(gui_window *win, gui_rc tbl, gui_rc *rows,
                             int total, int row_h, int *top, int *follow,
                             gui_ui *ui, int handle_keys)
{
    int bar_w = 12;
    int visible;

    *rows = gui_mkrc(tbl.x + 1, tbl.y + 1, tbl.w - 2, tbl.h - 2);
    visible = rows->h / row_h;
    if (visible < 1)
        visible = 1;

    if (follow && *follow && total > visible)
        *top = total - visible;

    if (total > visible) {
        int prev_top = *top;
        rows->w = tbl.w - 2 - bar_w;
        if (gui_scrollbar(win, gui_mkrc(tbl.x + tbl.w - bar_w - 1, tbl.y + 1,
                                        bar_w, tbl.h - 2),
                          total, visible, top, ui)) {
            if (follow)
                *follow = (*top < prev_top) ? 0 : (*top >= total - visible);
        }
    } else {
        *top = 0;
    }

    if (ui->wheel && gui_hit(tbl, ui->mx, ui->my)) {
        *top -= ui->wheel * 3;
        ui->wheel = 0;
        if (follow)
            *follow = (*top >= total - visible);
    }

    if (handle_keys && ui->key) {
        int handled = 1;
        switch (ui->key) {
        case KEY_UP:   (*top)--; break;
        case KEY_DOWN: (*top)++; break;
        case KEY_PGUP: *top -= visible; break;
        case KEY_PGDN: *top += visible; break;
        case KEY_HOME: *top = 0; break;
        case KEY_END:  *top = total; break;
        default:       handled = 0; break;
        }
        if (handled) {
            if (follow)
                *follow = (*top >= total - visible);
            ui->key = 0;
        }
    }

    if (*top > total - visible)
        *top = total - visible;
    if (*top < 0)
        *top = 0;
}

static int fd_kpi_pill(gui_window *win, int x, int y, const char *text,
                       uint32_t dot_col, int active, gui_ui *ui)
{
    int tw = gui_measure_text_aa(text, FONT_SMALL, FONT_BOLD, FONT_ROMAN, 0);
    gui_rc r = gui_mkrc(x, y, tw + 30, 26);
    int hov = gui_hit(r, ui->mx, ui->my);

    gui_fill_round_rect(win, r, 13,
                        active ? gui_mix(dot_col, GUI_SURFACE, 195)
                               : (hov ? GUI_PANEL_HI : GUI_SURFACE_ALT));
    gui_stroke_round_rect(win, r, 13, 1,
                          active ? dot_col : (hov ? GUI_EDGE : GUI_BORDER));
    gui_disc(win, r.x + 12, r.y + r.h / 2, 4, dot_col);
    gui_draw_text_aa(win, r.x + 21, r.y + 7, text, FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT, GUI_TRANSPARENT);
    return clicked(ui, r);
}

static void paint_flight_events(gui_window *win, gui_ui *ui, int pad,
                                int tbl_hdr_y, int tbl_bottom, int *shown_out)
{
    int w = win->w;
    int row_h = 22;
    int nfilt = 0;
    const char *want = flight_cat_filter >= 0 ? fd_cat_names[flight_cat_filter] : 0;

    for (int i = 0; i < nfd_events; i++) {
        if (want && strcmp(fd_events[i].cat, want) != 0)
            continue;
        fd_ev_filtered[nfilt++] = i;
    }
    *shown_out = nfilt;

    gui_rc tbl = gui_mkrc(pad, tbl_hdr_y + 26, w - 2 * pad,
                          tbl_bottom - (tbl_hdr_y + 26));
    gui_rc rows;
    int col_tick = tbl.x + 10;
    int col_cpu = tbl.x + 88;
    int col_pid = tbl.x + 138;
    int col_cat = tbl.x + 188;
    int col_ev = tbl.x + 258;
    int col_arg0 = tbl.x + 382;
    int col_det = tbl.x + 492;

    gui_fill_round_rect(win, gui_mkrc(pad, tbl_hdr_y, w - 2 * pad, 24), 6,
                        GUI_PANEL);
    gui_draw_text_aa(win, col_tick, tbl_hdr_y + 6, "TICK", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_cpu, tbl_hdr_y + 6, "CPU", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_pid, tbl_hdr_y + 6, "PID", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_cat, tbl_hdr_y + 6, "CAT", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_ev, tbl_hdr_y + 6, "EVENT", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_arg0, tbl_hdr_y + 6, "ARG0", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_det, tbl_hdr_y + 6, "DETAIL", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
    gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

    fd_scroll_handle(win, tbl, &rows, nfilt, row_h, &fd_ev_top,
                     &flight_follow, ui, 1);

    int visible = rows.h / row_h;
    if (visible < 1)
        visible = 1;

    gui_push_clip(win, rows);
    if (nfilt == 0) {
        gui_draw_text_aa(win, rows.x + 14, rows.y + 14,
                         "No flight recorder events matching filter.",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    } else {
        for (int i = 0; i < visible; i++) {
            int fi = fd_ev_top + i;
            if (fi >= nfilt)
                break;
            const struct fd_event_row *e = &fd_events[fd_ev_filtered[fi]];
            int y = rows.y + i * row_h;
            char tick_s[24], cpu_s[12], pid_s[12];
            uint32_t ccol = flight_cat_color(e->cat);

            if (fi & 1)
                gui_rect(win, rows.x, y, rows.w, row_h,
                         gui_shade(GUI_SUNKEN, 4));

            snprintf(tick_s, sizeof(tick_s), "%llu",
                     (unsigned long long)e->tick);
            snprintf(cpu_s, sizeof(cpu_s), "cpu%d", e->cpu);
            snprintf(pid_s, sizeof(pid_s), "%d", e->pid);

            gui_draw_text_mono(win, col_tick, y + 3, tick_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_cpu, y + 3, cpu_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_pid, y + 3, pid_s,
                               GUI_TEXT, GUI_TRANSPARENT);
            fd_draw_pill_cell(win, col_cat, y + 2, 54, row_h - 4, e->cat, ccol);
            gui_draw_text_aa_run(win, col_ev, y + 5, e->event, -1, FONT_SMALL,
                                 FONT_BOLD, FONT_ROMAN, GUI_TEXT,
                                 GUI_TRANSPARENT, col_arg0 - col_ev - 8, 1);
            gui_draw_text_mono(win, col_arg0, y + 3, e->arg0,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_aa_run(win, col_det, y + 5, e->detail, -1, FONT_SMALL,
                                 FONT_REGULAR, FONT_ROMAN, ccol,
                                 GUI_TRANSPARENT,
                                 rows.x + rows.w - col_det - 8, 1);
        }
    }
    gui_pop_clip(win);
}

static void paint_flight_sockets(gui_window *win, gui_ui *ui, int pad,
                                 int tbl_hdr_y, int tbl_bottom)
{
    int w = win->w;
    int row_h = 24;
    gui_rc tbl = gui_mkrc(pad, tbl_hdr_y + 26, w - 2 * pad,
                          tbl_bottom - (tbl_hdr_y + 26));
    gui_rc rows;
    int col_slot = tbl.x + 10;
    int col_pid = tbl.x + 64;
    int col_st = tbl.x + 124;
    int col_lp = tbl.x + 252;
    int col_peer = tbl.x + 332;
    int col_rx = tbl.x + 530;
    int col_tx = tbl.x + 626;

    gui_fill_round_rect(win, gui_mkrc(pad, tbl_hdr_y, w - 2 * pad, 24), 6,
                        GUI_PANEL);
    gui_draw_text_aa(win, col_slot, tbl_hdr_y + 6, "SLOT", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_pid, tbl_hdr_y + 6, "PID", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_st, tbl_hdr_y + 6, "STATE", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_lp, tbl_hdr_y + 6, "LOCAL", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_peer, tbl_hdr_y + 6, "PEER", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_rx, tbl_hdr_y + 6, "RX BYTES", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_draw_text_aa(win, col_tx, tbl_hdr_y + 6, "TX BYTES", FONT_SMALL, FONT_BOLD,
                     FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);

    gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
    gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

    fd_scroll_handle(win, tbl, &rows, nfd_sockets, row_h, &fd_sock_top,
                     0, ui, 1);

    int visible = rows.h / row_h;
    if (visible < 1)
        visible = 1;

    gui_push_clip(win, rows);
    if (nfd_sockets == 0) {
        gui_draw_text_aa(win, rows.x + 14, rows.y + 14,
                         "No active TCP sockets (/dev/sockets).",
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    } else {
        for (int i = 0; i < visible; i++) {
            int idx = fd_sock_top + i;
            if (idx >= nfd_sockets)
                break;
            const struct fd_socket_row *s = &fd_sockets[idx];
            int y = rows.y + i * row_h;
            char slot_s[12], pid_s[12], lp_s[12], peer_s[32], rx_s[16], tx_s[16];

            if (idx & 1)
                gui_rect(win, rows.x, y, rows.w, row_h,
                         gui_shade(GUI_SUNKEN, 4));
            snprintf(slot_s, sizeof(slot_s), "#%d", s->slot);
            snprintf(pid_s, sizeof(pid_s), "%d", s->pid);
            snprintf(lp_s, sizeof(lp_s), ":%d", s->local_port);
            snprintf(peer_s, sizeof(peer_s), "%s:%d", s->peer_ip, s->peer_port);
            snprintf(rx_s, sizeof(rx_s), "%d", s->rx_bytes);
            snprintf(tx_s, sizeof(tx_s), "%d", s->tx_bytes);

            gui_draw_text_mono(win, col_slot, y + 4, slot_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_pid, y + 4, pid_s,
                               GUI_TEXT, GUI_TRANSPARENT);
            fd_draw_pill_cell(win, col_st, y + 3, 108, row_h - 6, s->state,
                              socket_state_color(s->state));
            gui_draw_text_mono(win, col_lp, y + 4, lp_s,
                               GUI_ACCENT, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_peer, y + 4, peer_s,
                               GUI_TEXT, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_rx, y + 4, rx_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_tx, y + 4, tx_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
        }
    }
    gui_pop_clip(win);
}

static void paint_flight_caps_parcels(gui_window *win, gui_ui *ui, int pad,
                                      int top_y, int bottom_y)
{
    int w = win->w;
    int avail_h = bottom_y - top_y;
    int gap = 10;
    int caps_sec_h = (avail_h - gap) * 54 / 100;
    int parc_sec_h = avail_h - gap - caps_sec_h;
    int row_h = 22;

    /* Top panel: /dev/caps */
    {
        gui_rc hdr = gui_mkrc(pad, top_y, w - 2 * pad, 24);
        gui_rc tbl = gui_mkrc(pad, top_y + 26, w - 2 * pad, caps_sec_h - 26);
        gui_rc rows;
        int col_pid = tbl.x + 10;
        int col_uid = tbl.x + 68;
        int col_mask = tbl.x + 122;
        int col_caps = tbl.x + 204;
        int col_unveil = tbl.x + 516;

        gui_fill_round_rect(win, hdr, 6, GUI_PANEL);
        gui_draw_text_aa(win, col_pid, hdr.y + 6, "PID", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_uid, hdr.y + 6, "UID", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_mask, hdr.y + 6, "MASK", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_caps, hdr.y + 6, "CAPABILITIES (/dev/caps)",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_unveil, hdr.y + 6, "UNVEIL", FONT_SMALL,
                         FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);

        gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
        gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

        fd_scroll_handle(win, tbl, &rows, nfd_caps, row_h, &fd_caps_top,
                         0, ui, 1);

        int visible = rows.h / row_h;
        if (visible < 1)
            visible = 1;

        gui_push_clip(win, rows);
        for (int i = 0; i < visible; i++) {
            int idx = fd_caps_top + i;
            if (idx >= nfd_caps)
                break;
            const struct fd_cap_row *c = &fd_caps[idx];
            int y = rows.y + i * row_h;
            char pid_s[12], uid_s[12];
            uint32_t cap_col = strcmp(c->caps, "all") == 0 ? GUI_WARN
                             : (strcmp(c->caps, "none") == 0 ? GUI_ERROR
                                                             : GUI_OK);
            uint32_t unv_col = strcmp(c->unveil, "-") == 0 ? GUI_TEXT_DIM
                                                           : GUI_ACCENT;

            if (idx & 1)
                gui_rect(win, rows.x, y, rows.w, row_h,
                         gui_shade(GUI_SUNKEN, 4));
            snprintf(pid_s, sizeof(pid_s), "%d", c->pid);
            snprintf(uid_s, sizeof(uid_s), "%u", c->uid);

            gui_draw_text_mono(win, col_pid, y + 3, pid_s,
                               GUI_TEXT, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_uid, y + 3, uid_s,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_mono(win, col_mask, y + 3, c->cap_mask,
                               GUI_TEXT_DIM, GUI_TRANSPARENT);
            gui_draw_text_aa_run(win, col_caps, y + 5, c->caps, -1, FONT_SMALL,
                                 FONT_BOLD, FONT_ROMAN, cap_col,
                                 GUI_TRANSPARENT, col_unveil - col_caps - 10, 1);
            gui_draw_text_aa_run(win, col_unveil, y + 5, c->unveil, -1,
                                 FONT_SMALL, FONT_REGULAR, FONT_ROMAN, unv_col,
                                 GUI_TRANSPARENT,
                                 rows.x + rows.w - col_unveil - 8, 1);
        }
        gui_pop_clip(win);
    }

    /* Bottom panel: /dev/parcels */
    {
        int py = top_y + caps_sec_h + gap;
        gui_rc hdr = gui_mkrc(pad, py, w - 2 * pad, 24);
        gui_rc tbl = gui_mkrc(pad, py + 26, w - 2 * pad, parc_sec_h - 26);
        gui_rc rows;
        int col_id = tbl.x + 10;
        int col_uid = tbl.x + 68;
        int col_pid = tbl.x + 120;
        int col_type = tbl.x + 174;
        int col_size = tbl.x + 252;
        int col_mime = tbl.x + 324;
        int col_lbl = tbl.x + 480;

        gui_fill_round_rect(win, hdr, 6, GUI_PANEL);
        gui_draw_text_aa(win, col_id, hdr.y + 6, "ID", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_uid, hdr.y + 6, "UID", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_pid, hdr.y + 6, "PID", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_type, hdr.y + 6, "TYPE", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_size, hdr.y + 6, "SIZE", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_mime, hdr.y + 6, "MIME", FONT_SMALL, FONT_BOLD,
                         FONT_ROMAN, GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_draw_text_aa(win, col_lbl, hdr.y + 6, "LABEL (/dev/parcels)",
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN, GUI_TEXT_DIM,
                         GUI_TRANSPARENT);

        gui_fill_round_rect(win, tbl, 8, GUI_SUNKEN);
        gui_stroke_round_rect(win, tbl, 8, 1, GUI_BORDER);

        fd_scroll_handle(win, tbl, &rows, nfd_parcels, row_h, &fd_parcels_top,
                         0, ui, 0);

        int visible = rows.h / row_h;
        if (visible < 1)
            visible = 1;

        gui_push_clip(win, rows);
        if (nfd_parcels == 0) {
            gui_draw_text_aa(win, rows.x + 14, rows.y + 12,
                             "No active Talon parcels (/dev/parcels).",
                             FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT);
        } else {
            for (int i = 0; i < visible; i++) {
                int idx = fd_parcels_top + i;
                if (idx >= nfd_parcels)
                    break;
                const struct fd_parcel_row *p = &fd_parcels[idx];
                int y = rows.y + i * row_h;
                char id_s[12], uid_s[12], pid_s[12], sz_s[16];
                uint32_t tcol = parcel_type_color(p->type);

                if (idx & 1)
                    gui_rect(win, rows.x, y, rows.w, row_h,
                             gui_shade(GUI_SUNKEN, 4));
                snprintf(id_s, sizeof(id_s), "#%d", p->id);
                snprintf(uid_s, sizeof(uid_s), "%d", p->uid);
                snprintf(pid_s, sizeof(pid_s), "%d", p->pid);
                snprintf(sz_s, sizeof(sz_s), "%dB", p->size);

                gui_draw_text_mono(win, col_id, y + 3, id_s,
                                   GUI_TEXT_DIM, GUI_TRANSPARENT);
                gui_draw_text_mono(win, col_uid, y + 3, uid_s,
                                   GUI_TEXT_DIM, GUI_TRANSPARENT);
                gui_draw_text_mono(win, col_pid, y + 3, pid_s,
                                   GUI_TEXT, GUI_TRANSPARENT);
                fd_draw_pill_cell(win, col_type, y + 2, 62, row_h - 4,
                                  p->type, tcol);
                gui_draw_text_mono(win, col_size, y + 3, sz_s,
                                   GUI_TEXT_DIM, GUI_TRANSPARENT);
                gui_draw_text_aa_run(win, col_mime, y + 5, p->mime, -1,
                                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                     GUI_TEXT_DIM, GUI_TRANSPARENT,
                                     col_lbl - col_mime - 8, 1);
                gui_draw_text_aa_run(win, col_lbl, y + 5, p->label, -1,
                                     FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                                     GUI_TEXT, GUI_TRANSPARENT,
                                     rows.x + rows.w - col_lbl - 8, 1);
            }
        }
        gui_pop_clip(win);
    }
}

static void paint_flight(gui_window *win, gui_ui *ui)
{
    static const char *const sub_labels[3] = {
        "Events", "Sockets", "Caps & Parcels"
    };
    int w = win->w, h = win->h;
    int pad = 16, kpi_y = HEADER_H + 10, bar_y = kpi_y + 34;
    int tbl_hdr_y = bar_y + 34, foot_h = 28;
    int tbl_bottom = h - foot_h - 6;
    int kx = pad, sx = pad, shown_events = nfd_events;
    char p1[32], p2[32], p3[32], foot[112];

    /* 1. Summary KPI Pills + Stream Controls */
    snprintf(p1, sizeof(p1), "Events: %d", nfd_events);
    snprintf(p2, sizeof(p2), "Sockets: %d", nfd_sockets);
    snprintf(p3, sizeof(p3), "Parcels: %d", nfd_parcels);

    if (fd_kpi_pill(win, kx, kpi_y, p1, 0x38BDF8U,
                    flight_subview == FD_VIEW_EVENTS, ui)) {
        flight_subview = FD_VIEW_EVENTS;
        refresh_flight_deck(1);
    }
    kx += gui_measure_text_aa(p1, FONT_SMALL, FONT_BOLD, FONT_ROMAN, 0) + 38;

    if (fd_kpi_pill(win, kx, kpi_y, p2,
                    nfd_sockets > 0 ? 0x34D399U : GUI_TEXT_DIM,
                    flight_subview == FD_VIEW_SOCKETS, ui)) {
        flight_subview = FD_VIEW_SOCKETS;
        refresh_flight_deck(1);
    }
    kx += gui_measure_text_aa(p2, FONT_SMALL, FONT_BOLD, FONT_ROMAN, 0) + 38;

    if (fd_kpi_pill(win, kx, kpi_y, p3,
                    nfd_parcels > 0 ? 0xFB923CU : GUI_TEXT_DIM,
                    flight_subview == FD_VIEW_CAPS_PARCELS, ui)) {
        flight_subview = FD_VIEW_CAPS_PARCELS;
        refresh_flight_deck(1);
    }

    if (flight_subview == FD_VIEW_EVENTS) {
        if (gui_button_ex(win, gui_mkrc(w - pad - 222, kpi_y, 68, 26),
                          "Follow", ui, 1, flight_follow))
            flight_follow = !flight_follow;
    }
    if (gui_button_ex(win, gui_mkrc(w - pad - 148, kpi_y, 70, 26),
                      flight_paused ? "Resume" : "Pause", ui, 1,
                      flight_paused))
        flight_paused = !flight_paused;
    if (gui_button(win, gui_mkrc(w - pad - 72, kpi_y, 72, 26),
                   "Refresh", ui))
        refresh_flight_deck(1);

    /* 2. Sub-view Switcher + Category Filter Pills */
    for (int i = 0; i < 3; i++) {
        int bw = gui_text_w(sub_labels[i]) + 18;
        if (gui_button_ex(win, gui_mkrc(sx, bar_y, bw, 26), sub_labels[i],
                          ui, 1, flight_subview == i)) {
            flight_subview = i;
            refresh_flight_deck(1);
        }
        sx += bw + 6;
    }

    if (flight_subview == FD_VIEW_EVENTS) {
        int fx = sx + 8;
        int aw = gui_text_w("ALL") + 16;
        if (fx + aw <= w - pad) {
            if (gui_button_ex(win, gui_mkrc(fx, bar_y, aw, 26), "ALL", ui, 1,
                              flight_cat_filter < 0)) {
                flight_cat_filter = -1;
                fd_ev_top = 0;
            }
            fx += aw + 5;
        }
        for (int i = 0; i < FD_NUM_CATS; i++) {
            int cw = gui_measure_text_aa(fd_cat_names[i], FONT_SMALL, FONT_BOLD,
                                         FONT_ROMAN, 0) + 14;
            gui_rc cr = gui_mkrc(fx, bar_y, cw, 26);
            int act = (flight_cat_filter == i);
            int hov = gui_hit(cr, ui->mx, ui->my);
            uint32_t ccol = flight_cat_color(fd_cat_names[i]);

            if (fx + cw > w - pad)
                break;
            gui_fill_round_rect(win, cr, 6,
                                act ? gui_mix(ccol, GUI_SURFACE, 155)
                                    : (hov ? GUI_PANEL_HI : GUI_SUNKEN));
            gui_stroke_round_rect(win, cr, 6, 1,
                                  act ? ccol : (hov ? GUI_EDGE : GUI_BORDER));
            gui_draw_text_aa(win, cr.x + 7, cr.y + 7, fd_cat_names[i],
                             FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                             act ? GUI_WHITE : ccol, GUI_TRANSPARENT);
            if (clicked(ui, cr)) {
                flight_cat_filter = i;
                fd_ev_top = 0;
            }
            fx += cw + 5;
        }
    }

    /* 3. Active Sub-View Table */
    if (flight_subview == FD_VIEW_EVENTS)
        paint_flight_events(win, ui, pad, tbl_hdr_y, tbl_bottom, &shown_events);
    else if (flight_subview == FD_VIEW_SOCKETS)
        paint_flight_sockets(win, ui, pad, tbl_hdr_y, tbl_bottom);
    else
        paint_flight_caps_parcels(win, ui, pad, tbl_hdr_y, tbl_bottom);

    /* 4. Footer */
    gui_rect(win, 0, h - foot_h, w, foot_h, GUI_PANEL);
    gui_rect(win, 0, h - foot_h, w, 1, GUI_EDGE);
    if (flight_subview == FD_VIEW_EVENTS) {
        snprintf(foot, sizeof(foot),
                 "/dev/flight: %d / %d events   |   Caps: %d   Sockets: %d   Parcels: %d   |   %s",
                 shown_events, nfd_events, nfd_caps, nfd_sockets, nfd_parcels,
                 flight_paused ? "PAUSED" : "LIVE @krec");
    } else if (flight_subview == FD_VIEW_SOCKETS) {
        snprintf(foot, sizeof(foot),
                 "/dev/sockets: %d active TCP connections   |   Events: %d   Parcels: %d   |   %s",
                 nfd_sockets, nfd_events, nfd_parcels,
                 flight_paused ? "PAUSED" : "LIVE @krec");
    } else {
        snprintf(foot, sizeof(foot),
                 "/dev/caps: %d tasks   |   /dev/parcels: %d parcels   |   Events: %d   |   %s",
                 nfd_caps, nfd_parcels, nfd_events,
                 flight_paused ? "PAUSED" : "LIVE @krec");
    }
    gui_draw_text_aa(win, pad, h - 20, foot, FONT_SMALL, FONT_REGULAR,
                     FONT_ROMAN,
                     flight_paused ? GUI_WARN : GUI_TEXT_DIM,
                     GUI_TRANSPARENT);
}

static void paint(gui_window *win, gui_ui *ui)
{
    gui_clear(win, GUI_SURFACE);
    paint_header(win, ui);
    if (active_tab == TAB_RESOURCES)
        paint_resources(win, ui);
    else if (active_tab == TAB_PROCESSES)
        paint_processes(win, ui);
    else if (active_tab == TAB_PULSES)
        paint_pulses(win, ui);
    else
        paint_flight(win, ui);
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
    refresh_flight_deck(1);

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
            } else if (ev.type == KEV_KEY) {
                if (ev.key == '1') {
                    active_tab = TAB_RESOURCES;
                } else if (ev.key == '2') {
                    active_tab = TAB_PROCESSES;
                    check_sight();
                } else if (ev.key == '3') {
                    active_tab = TAB_PULSES;
                } else if (ev.key == '4') {
                    active_tab = TAB_FLIGHT;
                    refresh_flight_deck(1);
                } else if (active_tab == TAB_FLIGHT) {
                    if (ev.key == 'e' || ev.key == 'E') {
                        flight_subview = FD_VIEW_EVENTS;
                        refresh_flight_deck(1);
                    } else if (ev.key == 's' || ev.key == 'S') {
                        flight_subview = FD_VIEW_SOCKETS;
                        refresh_flight_deck(1);
                    } else if (ev.key == 'c' || ev.key == 'C') {
                        flight_subview = FD_VIEW_CAPS_PARCELS;
                        refresh_flight_deck(1);
                    }
                }
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
            if (active_tab == TAB_FLIGHT)
                refresh_flight_deck(0);
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
