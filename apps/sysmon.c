/* sysmon.c - graphical process and resource monitor. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define WIN_W 680
#define WIN_H 500
#define MAX_PROCS 128

static struct k_psinfo procs[MAX_PROCS];
static int nprocs;
static gui_list process_list;
static int confirm_pid = -1;
static char status[96] = "Live process list";

static const char *state_name(int state)
{
    switch (state) {
    case K_STATE_RUNNABLE: return "ready";
    case K_STATE_RUNNING:  return "running";
    case K_STATE_SLEEPING: return "sleeping";
    case K_STATE_ZOMBIE:   return "zombie";
    case K_STATE_STOPPED:  return "stopped";
    default:               return "?";
    }
}

static const char *process_text(void *ctx, int index)
{
    static char line[128];
    struct k_psinfo *p;

    (void)ctx;
    if (index < 0 || index >= nprocs)
        return "";
    p = &procs[index];
    snprintf(line, sizeof(line), "%5d  %-28s  %-9s  uid %-4u  ppid %d",
             p->pid, p->name, state_name(p->state), p->uid, p->ppid);
    return line;
}

static uint32_t process_tint(void *ctx, int index)
{
    (void)ctx;
    if (index < 0 || index >= nprocs)
        return GUI_TEXT;
    if (procs[index].state == K_STATE_ZOMBIE)
        return GUI_WARN;
    if (procs[index].state == K_STATE_STOPPED)
        return GUI_ERROR;
    if (procs[index].state == K_STATE_RUNNING)
        return GUI_OK;
    return GUI_TEXT;
}

static void refresh_processes(void)
{
    int selected_pid = -1;

    if (process_list.sel >= 0 && process_list.sel < nprocs)
        selected_pid = procs[process_list.sel].pid;
    nprocs = 0;
    while (nprocs < MAX_PROCS && psinfo(nprocs, &procs[nprocs]) == 0)
        nprocs++;
    process_list.count = nprocs;
    if (selected_pid >= 0) {
        process_list.sel = -1;
        for (int i = 0; i < nprocs; i++)
            if (procs[i].pid == selected_pid) {
                process_list.sel = i;
                break;
            }
    }
    if (process_list.sel < 0 && nprocs)
        process_list.sel = 0;
}

static void metric_card(gui_window *win, int x, const char *title,
                        const char *value, enum gui_icon icon,
                        int percent, uint32_t color)
{
    gui_rc r = gui_mkrc(x, 58, 198, 92);

    gui_card(win, r);
    gui_round_rect(win, gui_mkrc(r.x + 14, r.y + 14, 42, 42), 10,
                   gui_mix(color, GUI_SURFACE, 80));
    gui_icon(win, r.x + 23, r.y + 23, 24, icon, color);
    gui_text(win, r.x + 68, r.y + 15, title, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_text(win, r.x + 68, r.y + 39, value, GUI_TEXT,
             GUI_TRANSPARENT);
    if (percent >= 0)
        gui_progress(win, gui_mkrc(r.x + 14, r.y + 70, r.w - 28, 9),
                     percent, color);
}

static void paint(gui_window *win, gui_ui *ui)
{
    struct k_cpuinfo cpu;
    struct k_netinfo net;
    uint64_t total = 0, free = 0;
    uint64_t used = 0;
    int mem_pct = 0;
    char value[64], ip[16];
    gui_rc list_rc = gui_mkrc(18, 174, WIN_W - 36, 246);
    int selected = process_list.sel;

    gui_clear(win, GUI_SURFACE);
    gui_hgradient(win, gui_mkrc(0, 0, WIN_W, 42),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 28), GUI_PANEL);
    gui_icon(win, 14, 10, 22, GUI_ICON_MONITOR, GUI_ACCENT);
    gui_text(win, 46, 13, "System Monitor", GUI_TEXT, GUI_TRANSPARENT);
    snprintf(value, sizeof(value), "refresh 1s");
    gui_badge(win, gui_mkrc(WIN_W - 116, 9, 100, 24), value, GUI_OK);

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    snprintf(value, sizeof(value), "%u online", cpu.online);
    metric_card(win, 18, "Processors", value, GUI_ICON_MONITOR,
                -1, GUI_OK);

    if (meminfo(&total, &free) == 0 && total) {
        used = total - free;
        mem_pct = (int)((used * 100) / total);
    }
    snprintf(value, sizeof(value), "%llu / %llu MiB",
             (unsigned long long)(used / 1024),
             (unsigned long long)(total / 1024));
    metric_card(win, 241, "Memory", value, GUI_ICON_INFO,
                mem_pct, mem_pct > 85 ? GUI_ERROR : GUI_ACCENT);

    memset(&net, 0, sizeof(net));
    if (netinfo(&net) == 0 && net.up)
        snprintf(value, sizeof(value), "%s", ip_ntoa(net.ip, ip));
    else
        snprintf(value, sizeof(value), "offline");
    metric_card(win, 464, "Network", value, GUI_ICON_NETWORK,
                -1, net.up ? GUI_OK : GUI_TEXT_DIM);

    gui_text(win, 20, 156, "PID    NAME                          STATE      USER      PARENT",
             GUI_TEXT_DIM, GUI_TRANSPARENT);
    process_list.item = process_text;
    process_list.tint = process_tint;
    process_list.row_h = 24;
    gui_listbox(win, list_rc, &process_list, ui);

    gui_rect(win, 0, WIN_H - 64, WIN_W, 64, GUI_PANEL);
    gui_rect(win, 0, WIN_H - 64, WIN_W, 1, GUI_EDGE);
    gui_text(win, 18, WIN_H - 47, status, GUI_TEXT_DIM, GUI_TRANSPARENT);

    selected = process_list.sel;
    if (confirm_pid < 0) {
        int can_end = selected >= 0 && selected < nprocs &&
                      procs[selected].pid > 1 &&
                      procs[selected].pid != getpid();

        if (gui_button_ex(win, gui_mkrc(WIN_W - 142, WIN_H - 52, 124, 36),
                          "End process", ui, can_end, 0)) {
            confirm_pid = procs[selected].pid;
            snprintf(status, sizeof(status), "End %s (pid %d)?",
                     procs[selected].name, confirm_pid);
        }
    } else {
        if (gui_button(win, gui_mkrc(WIN_W - 190, WIN_H - 52, 78, 36),
                       "Confirm", ui)) {
            if (kill(confirm_pid, SIGTERM) == 0)
                snprintf(status, sizeof(status), "SIGTERM sent to pid %d",
                         confirm_pid);
            else
                snprintf(status, sizeof(status), "Cannot signal pid %d",
                         confirm_pid);
            confirm_pid = -1;
        }
        if (gui_button(win, gui_mkrc(WIN_W - 104, WIN_H - 52, 86, 36),
                       "Cancel", ui)) {
            confirm_pid = -1;
            snprintf(status, sizeof(status), "Cancelled");
        }
    }
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    unsigned long next_refresh = 0;
    int dirty = 1;

    (void)argc;
    (void)argv;
    memset(&ui, 0, sizeof(ui));
    memset(&process_list, 0, sizeof(process_list));
    process_list.sel = -1;
    refresh_processes();

    win = gui_open("System Monitor", 170, 100, WIN_W, WIN_H, 0);
    if (!win) {
        printf("sysmon: no window manager available\n");
        return 1;
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, 100);

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
        if (uptime_ms() >= next_refresh) {
            next_refresh = uptime_ms() + 1000;
            refresh_processes();
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
