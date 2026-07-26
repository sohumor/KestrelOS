/* gfiles.c - a graphical file browser.
 *
 * A path bar, a scrollable listing of the current directory, a status
 * line describing the selection (size, owner, mode, modification time)
 * and buttons to open, edit or delete. Double-clicking a directory
 * enters it, ".." goes up, and deletion asks first.
 *
 * usage: gfiles [path]
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define WIN_W       660
#define WIN_H       470
#define MAX_ENTRIES 512
#define MAX_PATH    256
#define ROW_H       (GUI_FONT_H + 4)

struct entry {
    char name[64];
    unsigned int size;
    unsigned int mode;
    unsigned int uid, gid, mtime;
    int is_dir;
};

static struct entry entries[MAX_ENTRIES];
static int nentries;
static char cwd[MAX_PATH] = "/";
static char pathbuf[MAX_PATH];
static char status[MAX_PATH + 96];      /* holds a full path plus detail */
static int confirm_delete;

static gui_list list;
static gui_textbox_state pathbox;
static gui_menu_state sortmenu;
static int last_sel = -2;       /* forces the first describe() */

enum { SORT_NAME, SORT_SIZE, SORT_TIME };
static int sort_mode = SORT_NAME;
static int dirs_first = 1;

/* --------------------------------------------------------- path helpers */

static void join(const char *dir, const char *name, char *out,
                 unsigned long outsz)
{
    if (name[0] == '/')
        snprintf(out, outsz, "%s", name);
    else if (dir[1] == '\0')
        snprintf(out, outsz, "/%s", name);
    else
        snprintf(out, outsz, "%s/%s", dir, name);
}

/* Collapse "." and ".." so the path bar always shows a canonical path. */
static void normalize(char *path)
{
    char out[MAX_PATH];
    char name[MAX_PATH];
    const char *p = path;
    int len = 1, i;

    out[0] = '/';
    out[1] = '\0';
    while (*p) {
        int n = 0;

        while (*p == '/')
            p++;
        while (*p && *p != '/' && n < (int)sizeof(name) - 1)
            name[n++] = *p++;
        name[n] = '\0';
        if (n == 0)
            break;
        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            while (len > 1 && out[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            out[len] = '\0';
            continue;
        }
        if (len > 1 && len < (int)sizeof(out) - 1)
            out[len++] = '/';
        for (i = 0; name[i] && len < (int)sizeof(out) - 1; i++)
            out[len++] = name[i];
        out[len] = '\0';
    }
    strncpy(path, out, MAX_PATH - 1);
    path[MAX_PATH - 1] = '\0';
}

static void mode_string(unsigned int mode, char *out)
{
    static const char bits[3] = { 'r', 'w', 'x' };
    int i;

    for (i = 0; i < 9; i++)
        out[i] = (mode & (0400 >> i)) ? bits[i % 3] : '-';
    out[9] = '\0';
}

/* ------------------------------------------------------------- listing */

/* True if a belongs before b under the current sort order. */
static int before(const struct entry *a, const struct entry *b)
{
    if (dirs_first && a->is_dir != b->is_dir)
        return a->is_dir;
    switch (sort_mode) {
    case SORT_SIZE:
        if (a->size != b->size)
            return a->size > b->size;
        break;
    case SORT_TIME:
        if (a->mtime != b->mtime)
            return a->mtime > b->mtime;
        break;
    default:
        break;
    }
    return strcmp(a->name, b->name) < 0;
}

/* Insertion sort: these listings are tens of entries, not thousands. */
static void sort_entries(void)
{
    int i, j;

    for (i = 1; i < nentries; i++) {
        struct entry key = entries[i];

        for (j = i - 1; j >= 0 && before(&key, &entries[j]); j--)
            entries[j + 1] = entries[j];
        entries[j + 1] = key;
    }
}

static void load_dir(void)
{
    struct k_dirent de;
    int i = 0, have_parent = 0;

    nentries = 0;
    confirm_delete = 0;
    while (nentries < MAX_ENTRIES && readdir_at(cwd, i++, &de) == 0) {
        if (strcmp(de.name, ".") == 0)
            continue;
        if (strcmp(de.name, "..") == 0) {
            if (cwd[1] == '\0')
                continue;               /* no parent above the root */
            have_parent = 1;
        }
        strncpy(entries[nentries].name, de.name,
                sizeof(entries[0].name) - 1);
        entries[nentries].name[sizeof(entries[0].name) - 1] = '\0';
        entries[nentries].size = de.size;
        entries[nentries].mode = de.mode;
        entries[nentries].uid = de.uid;
        entries[nentries].gid = de.gid;
        entries[nentries].mtime = de.mtime;
        entries[nentries].is_dir = de.is_dir != 0;
        nentries++;
    }

    if (!have_parent && cwd[1] != '\0' && nentries < MAX_ENTRIES) {
        memset(&entries[nentries], 0, sizeof(entries[0]));
        strcpy(entries[nentries].name, "..");
        entries[nentries].is_dir = 1;
        nentries++;
    }
    sort_entries();

    list.count = nentries;
    list.sel = nentries ? 0 : -1;
    list.top = 0;
    if (i <= 1 && nentries == 0)
        snprintf(status, sizeof(status), "%s: cannot read directory", cwd);
    else
        snprintf(status, sizeof(status), "%d entr%s", nentries,
                 nentries == 1 ? "y" : "ies");

    strncpy(pathbuf, cwd, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';
    pathbox.len = (int)strlen(pathbuf);
    pathbox.caret = pathbox.len;
    pathbox.scroll = 0;
}

static const char *item_text(void *ctx, int index)
{
    static char buf[128];
    const struct entry *e;

    (void)ctx;
    if (index < 0 || index >= nentries)
        return "";
    e = &entries[index];
    if (e->is_dir)
        snprintf(buf, sizeof(buf), "%c %-44s %8s", 'd', e->name, "<dir>");
    else
        snprintf(buf, sizeof(buf), "%c %-44s %8u", '-', e->name, e->size);
    return buf;
}

static uint32_t item_tint(void *ctx, int index)
{
    (void)ctx;
    if (index < 0 || index >= nentries)
        return GUI_TEXT;
    if (entries[index].is_dir)
        return GUI_ACCENT;
    if (entries[index].mode & (K_IXUSR | K_IXGRP | K_IXOTH))
        return GUI_OK;
    return GUI_TEXT;
}

static void describe(int index)
{
    const struct entry *e;
    struct gui_tm tm;
    char perms[10];

    if (index < 0 || index >= nentries) {
        snprintf(status, sizeof(status), "%d entries", nentries);
        return;
    }
    e = &entries[index];
    mode_string(e->mode, perms);
    if (e->mtime) {
        gui_gmtime(e->mtime, &tm);
        snprintf(status, sizeof(status),
                 "%s  %s  uid %u gid %u  %u bytes  %d %s %d %02d:%02d",
                 e->name, perms, e->uid, e->gid, e->size, tm.day,
                 gui_month_name(tm.mon), tm.year, tm.hour, tm.min);
    } else {
        snprintf(status, sizeof(status), "%s  %s  uid %u gid %u  %u bytes",
                 e->name, perms, e->uid, e->gid, e->size);
    }
}

/* ------------------------------------------------------------- actions */

static void enter_dir(const char *name)
{
    char next[MAX_PATH];

    join(cwd, name, next, sizeof(next));
    normalize(next);
    strncpy(cwd, next, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
    load_dir();
}

static void open_selection(int index)
{
    char path[MAX_PATH];
    char *argv[3];
    struct k_stat st;
    int pid;

    if (index < 0 || index >= nentries)
        return;
    if (entries[index].is_dir) {
        enter_dir(entries[index].name);
        return;
    }
    if (stat_("/bin/edit", &st) != 0) {
        snprintf(status, sizeof(status), "/bin/edit is not installed");
        return;
    }
    join(cwd, entries[index].name, path, sizeof(path));
    argv[0] = "/bin/edit";
    argv[1] = path;
    argv[2] = 0;
    pid = spawn("/bin/edit", argv);
    if (pid < 0)
        snprintf(status, sizeof(status), "cannot start the editor");
    else
        snprintf(status, sizeof(status),
                 "editing %s on the text console (pid %d)", path, pid);
}

static void delete_selection(void)
{
    char path[MAX_PATH];
    int sel = list.sel;

    if (sel < 0 || sel >= nentries)
        return;
    join(cwd, entries[sel].name, path, sizeof(path));
    if (unlink_(path) != 0) {
        snprintf(status, sizeof(status), "cannot delete %s", path);
        confirm_delete = 0;
        return;
    }
    load_dir();
    snprintf(status, sizeof(status), "deleted %s", path);
}

/* ------------------------------------------------------------ painting */

static void paint(gui_window *win, gui_ui *ui)
{
    static const char *const sort_items[3] = { "by name", "by size",
                                               "by time" };
    gui_rc list_rc = gui_mkrc(10, 46, WIN_W - 20, WIN_H - 46 - 66);
    gui_rc bar = gui_mkrc(10, 10, WIN_W - 20 - 4 * 72, 26);
    gui_ui list_ui;
    int activated, y, picked;

    gui_clear(win, GUI_SURFACE);

    /* path bar */
    if (gui_textbox(win, bar, &pathbox, ui)) {
        char want[MAX_PATH];
        struct k_stat st;

        snprintf(want, sizeof(want), "%s", pathbuf);
        normalize(want);
        if (stat_(want, &st) == 0 && st.is_dir) {
            strncpy(cwd, want, sizeof(cwd) - 1);
            cwd[sizeof(cwd) - 1] = '\0';
            load_dir();
        } else {
            snprintf(status, sizeof(status), "not a directory: %s", want);
        }
    }
    if (gui_button(win, gui_mkrc(WIN_W - 10 - 3 * 72 + 6, 10, 66, 26),
                   "Up", ui))
        enter_dir("..");
    if (gui_button(win, gui_mkrc(WIN_W - 10 - 2 * 72 + 6, 10, 66, 26),
                   "Home", ui))
        enter_dir("/");
    if (gui_button(win, gui_mkrc(WIN_W - 10 - 72 + 6, 10, 66, 26),
                   "Reload", ui))
        load_dir();

    /* listing. While the sort menu is dropped over the list, the list
     * must not also act on the click that lands on a menu item, so it
     * gets a copy of the input with the one-shot fields cleared. */
    list_ui = *ui;
    if (sortmenu.open) {
        list_ui.down = 0;
        list_ui.up = 0;
        list_ui.dbl = 0;
        list_ui.key = 0;
    }
    list.row_h = ROW_H;
    list.item = item_text;
    list.tint = item_tint;
    list.ctx = 0;
    activated = gui_listbox(win, list_rc, &list, &list_ui);
    if (!sortmenu.open)
        ui->key = list_ui.key;          /* propagate key consumption */
    if (activated >= 0) {
        open_selection(activated);
    } else if (list.sel != last_sel) {
        /* Only on a change, so an action's message is not immediately
         * overwritten by the description of the same selection. */
        last_sel = list.sel;
        describe(list.sel);
    }

    /* status line */
    y = WIN_H - 54;
    gui_rect(win, 10, y, WIN_W - 20, GUI_FONT_H + 6, gui_shade(GUI_SURFACE, -10));
    gui_clip(win, gui_mkrc(12, y, WIN_W - 24, GUI_FONT_H + 6));
    gui_text(win, 14, y + 3, status, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_unclip(win);

    /* actions */
    y = WIN_H - 34;
    if (gui_button(win, gui_mkrc(10, y, 90, 26), "Open", ui))
        open_selection(list.sel);
    if (gui_button_ex(win, gui_mkrc(106, y, 90, 26), "Edit", ui,
                      list.sel >= 0 && list.sel < nentries &&
                      !entries[list.sel].is_dir, 0))
        open_selection(list.sel);

    if (!confirm_delete) {
        if (gui_button_ex(win, gui_mkrc(202, y, 90, 26), "Delete", ui,
                          list.sel >= 0 && list.sel < nentries &&
                          strcmp(entries[list.sel].name, "..") != 0, 0)) {
            confirm_delete = 1;
            snprintf(status, sizeof(status), "delete %s - are you sure?",
                     entries[list.sel].name);
        }
    } else {
        gui_text(win, 202, y + 5, "sure?", GUI_WARN, GUI_TRANSPARENT);
        if (gui_button(win, gui_mkrc(258, y, 60, 26), "Yes", ui)) {
            delete_selection();
            confirm_delete = 0;
        }
        if (gui_button(win, gui_mkrc(324, y, 60, 26), "No", ui)) {
            confirm_delete = 0;
            snprintf(status, sizeof(status), "cancelled");
        }
    }

    if (gui_checkbox(win, WIN_W - 150, y + 5, "dirs first", &dirs_first, ui))
        sort_entries();

    /* Drawn last so the open drop-down covers the listing. */
    picked = gui_menu(win, gui_mkrc(WIN_W - 10 - 4 * 72 + 6, 10, 66, 26),
                      "Sort", sort_items, 3, &sortmenu, ui);
    if (picked >= 0) {
        sort_mode = picked;
        sort_entries();
        last_sel = -2;
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *win;
    gui_ui ui;
    struct k_stat st;
    int i, dirty = 1;

    memset(&ui, 0, sizeof(ui));
    memset(&list, 0, sizeof(list));
    pathbox.buf = pathbuf;
    pathbox.cap = sizeof(pathbuf);

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strncmp(arg, "--cwd=", 6) == 0)
            arg += 6;
        if (arg[0] == '\0')
            continue;
        snprintf(cwd, sizeof(cwd), "%s", arg);
    }
    normalize(cwd);
    if (stat_(cwd, &st) != 0 || !st.is_dir)
        strcpy(cwd, "/");

    win = gui_open("Files", 140, 100, WIN_W, WIN_H, 0);
    if (!win) {
        printf("gfiles: no window manager available\n");
        return 1;
    }

    load_dir();

    while (win->open) {
        int got = gui_next_event(win, &ev, 60);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == 27 && confirm_delete)
                confirm_delete = 0;
            dirty = 1;
        } else if (pathbox.focus) {
            dirty = 1;          /* keep the caret blinking */
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
