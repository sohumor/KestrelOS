/* gfiles.c - a graphical file browser for KestrelOS.
 *
 * Features:
 *   - Editable path bar + clickable breadcrumb segments (/ > doc > demos)
 *   - Colored file-type pill badges (DIR, JS, HTML, KREC, C, MD, IMG, BIN)
 *   - New Folder (mkdir_), Rename (rename_), Delete with confirmation banner
 *   - Talon [Copy Path] button (K_PARCEL_PATH)
 *   - Smart Open (.html -> browser, .js -> kjs, .md/.txt -> notes/edit,
 *     .krec -> sight/edit, .kpx/.bmp -> gpaint/edit, fallback -> edit)
 *   - Dynamic KEV_RESIZE reflow, KEV_MOUSE_WHEEL + scrollbar, and sorting
 *
 * usage: gfiles [path]
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define INIT_W      760
#define INIT_H      520
#define MAX_ENTRIES 512
#define MAX_PATH    256
#define ROW_H       26
#define MAX_KIDS    16

struct entry {
    char name[64];
    unsigned int size;
    unsigned int mode;
    unsigned int uid, gid, mtime;
    int is_dir;
};

enum action_mode {
    ACT_NONE = 0,
    ACT_MKDIR,
    ACT_RENAME,
    ACT_DELETE
};

static struct entry entries[MAX_ENTRIES];
static int nentries;
static char cwd[MAX_PATH] = "/";
static char pathbuf[MAX_PATH];
static char inputbuf[64];
static char status[MAX_PATH + 96];

static int sel_idx = -1;
static int top_idx = 0;
static int last_sel = -2;
static int act_mode = ACT_NONE;

static gui_textbox_state pathbox;
static gui_textbox_state inputbox;
static gui_menu_state sortmenu;

enum { SORT_NAME, SORT_SIZE, SORT_TIME };
static int sort_mode = SORT_NAME;
static int dirs_first = 1;

static int children[MAX_KIDS];
static int nchildren;

/* --------------------------------------------------------- path helpers */

static int lower_ch(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');

    if (!dot || dot == name)
        return 0;
    dot++;
    while (*dot && *ext) {
        if (lower_ch((unsigned char)*dot) != lower_ch((unsigned char)*ext))
            return 0;
        dot++;
        ext++;
    }
    return *dot == '\0' && *ext == '\0';
}

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

static void select_by_name(const char *name)
{
    for (int i = 0; i < nentries; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            sel_idx = i;
            last_sel = -2;
            return;
        }
    }
}

static void load_dir(void)
{
    struct k_dirent de;
    int i = 0, have_parent = 0;

    nentries = 0;
    act_mode = ACT_NONE;
    while (nentries < MAX_ENTRIES && readdir_at(cwd, i++, &de) == 0) {
        if (strcmp(de.name, ".") == 0)
            continue;
        if (strcmp(de.name, "..") == 0) {
            if (cwd[1] == '\0')
                continue;
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

    sel_idx = nentries ? 0 : -1;
    top_idx = 0;
    last_sel = -2;
    inputbox.focus = 0;
    pathbox.focus = 0;
    if (i <= 1 && nentries == 0)
        snprintf(status, sizeof(status), "%s: cannot read directory", cwd);
    else
        snprintf(status, sizeof(status), "%d entr%s in %s", nentries,
                 nentries == 1 ? "y" : "ies", cwd);

    strncpy(pathbuf, cwd, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';
    pathbox.len = (int)strlen(pathbuf);
    pathbox.caret = pathbox.len;
    pathbox.scroll = 0;
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

static void track_child(int pid)
{
    if (pid >= 0 && nchildren < MAX_KIDS)
        children[nchildren++] = pid;
}

static void enter_dir(const char *name)
{
    char next[MAX_PATH];

    join(cwd, name, next, sizeof(next));
    normalize(next);
    strncpy(cwd, next, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
    load_dir();
}

static void open_dir_abs(const char *abs_path)
{
    struct k_stat st;
    char want[MAX_PATH];

    snprintf(want, sizeof(want), "%s", abs_path);
    normalize(want);
    if (stat_(want, &st) == 0 && st.is_dir) {
        snprintf(cwd, sizeof(cwd), "%s", want);
        load_dir();
    } else {
        snprintf(status, sizeof(status), "not a directory: %s", want);
    }
}

static const char *pick_handler_app(const char *name, int force_edit)
{
    struct k_stat st;

    if (force_edit)
        return "/bin/edit";
    if (ext_is(name, "html") || ext_is(name, "htm")) {
        if (stat_("/bin/browser", &st) == 0)
            return "/bin/browser";
    } else if (ext_is(name, "js") || ext_is(name, "mjs")) {
        if (stat_("/bin/kjs", &st) == 0)
            return "/bin/kjs";
    } else if (ext_is(name, "md") || ext_is(name, "txt")) {
        if (stat_("/bin/notes", &st) == 0)
            return "/bin/notes";
    } else if (ext_is(name, "krec")) {
        if (stat_("/bin/sight", &st) == 0)
            return "/bin/sight";
    } else if (ext_is(name, "kpx") || ext_is(name, "bmp")) {
        if (stat_("/bin/gpaint", &st) == 0)
            return "/bin/gpaint";
    }
    return "/bin/edit";
}

static void open_selection(int index, int force_edit)
{
    char path[MAX_PATH];
    char *argv[3];
    struct k_stat st;
    const char *app;
    int pid;

    if (index < 0 || index >= nentries)
        return;
    if (entries[index].is_dir) {
        enter_dir(entries[index].name);
        return;
    }
    app = pick_handler_app(entries[index].name, force_edit);
    if (stat_(app, &st) != 0)
        app = "/bin/edit";
    if (stat_(app, &st) != 0) {
        snprintf(status, sizeof(status), "%s is not installed", app);
        return;
    }
    join(cwd, entries[index].name, path, sizeof(path));
    argv[0] = (char *)app;
    argv[1] = path;
    argv[2] = 0;
    pid = spawn(app, argv);
    if (pid < 0) {
        snprintf(status, sizeof(status), "cannot start %s", app);
    } else {
        track_child(pid);
        snprintf(status, sizeof(status), "opened %s with %s (pid %d)",
                 path, app, pid);
    }
}

static void copy_path_to_talon(void)
{
    char full[MAX_PATH];
    const char *label;

    if (sel_idx >= 0 && sel_idx < nentries &&
        strcmp(entries[sel_idx].name, "..") != 0) {
        join(cwd, entries[sel_idx].name, full, sizeof(full));
        label = entries[sel_idx].name;
    } else {
        snprintf(full, sizeof(full), "%s", cwd);
        label = cwd;
    }
    if (parcel_put(K_PARCEL_PATH, "text/x-path", label, full,
                   (uint32_t)strlen(full)) >= 0)
        snprintf(status, sizeof(status), "Copied path to Talon: %s", full);
    else
        snprintf(status, sizeof(status), "Failed copying path to Talon");
}

static void do_mkdir(void)
{
    char path[MAX_PATH];

    if (!inputbuf[0] || strchr(inputbuf, '/')) {
        snprintf(status, sizeof(status), "Invalid folder name");
        return;
    }
    join(cwd, inputbuf, path, sizeof(path));
    if (mkdir_(path) != 0) {
        snprintf(status, sizeof(status), "Cannot create folder %s", path);
        return;
    }
    load_dir();
    select_by_name(inputbuf);
    snprintf(status, sizeof(status), "Created folder %s", path);
}

static void do_rename(void)
{
    char oldp[MAX_PATH], newp[MAX_PATH], newname[64];

    if (sel_idx < 0 || sel_idx >= nentries ||
        strcmp(entries[sel_idx].name, "..") == 0)
        return;
    if (!inputbuf[0] || strchr(inputbuf, '/')) {
        snprintf(status, sizeof(status), "Invalid new name");
        return;
    }
    snprintf(newname, sizeof(newname), "%s", inputbuf);
    join(cwd, entries[sel_idx].name, oldp, sizeof(oldp));
    join(cwd, newname, newp, sizeof(newp));
    if (rename_(oldp, newp) != 0) {
        snprintf(status, sizeof(status), "Cannot rename %s", entries[sel_idx].name);
        return;
    }
    load_dir();
    select_by_name(newname);
    snprintf(status, sizeof(status), "Renamed to %s", newname);
}

static void do_delete(void)
{
    char path[MAX_PATH];
    int sel = sel_idx;

    if (sel < 0 || sel >= nentries ||
        strcmp(entries[sel].name, "..") == 0)
        return;
    join(cwd, entries[sel].name, path, sizeof(path));
    if (unlink_(path) != 0) {
        snprintf(status, sizeof(status), "Cannot delete %s", path);
        act_mode = ACT_NONE;
        return;
    }
    load_dir();
    snprintf(status, sizeof(status), "Deleted %s", path);
}

/* ----------------------------------------------------------- badges/ui */

static void entry_badge(const struct entry *e, const char **tag, uint32_t *col)
{
    if (e->is_dir) {
        *tag = "DIR";
        *col = GUI_ACCENT;
        return;
    }
    if (ext_is(e->name, "js") || ext_is(e->name, "mjs")) {
        *tag = "JS";
        *col = 0xEAB308U;
    } else if (ext_is(e->name, "html") || ext_is(e->name, "htm")) {
        *tag = "HTML";
        *col = 0x38BDF8U;
    } else if (ext_is(e->name, "krec")) {
        *tag = "KREC";
        *col = 0x14B8A6U;
    } else if (ext_is(e->name, "c") || ext_is(e->name, "h")) {
        *tag = "C";
        *col = 0xA78BFAU;
    } else if (ext_is(e->name, "md") || ext_is(e->name, "txt")) {
        *tag = ext_is(e->name, "md") ? "MD" : "TXT";
        *col = 0x4ADE80U;
    } else if (ext_is(e->name, "kpx") || ext_is(e->name, "bmp") ||
               ext_is(e->name, "png")) {
        *tag = "IMG";
        *col = 0xFB7185U;
    } else if (ext_is(e->name, "css")) {
        *tag = "CSS";
        *col = 0x60A5FAU;
    } else if (ext_is(e->name, "sh")) {
        *tag = "SH";
        *col = 0xF97316U;
    } else if (e->mode & (K_IXUSR | K_IXGRP | K_IXOTH)) {
        *tag = "BIN";
        *col = GUI_OK;
    } else {
        *tag = "FILE";
        *col = GUI_TEXT_DIM;
    }
}

static void draw_breadcrumbs(gui_window *win, gui_rc r, gui_ui *ui)
{
    char segs[16][64];
    char prefixes[16][MAX_PATH];
    int nseg = 0, x = r.x + 6;
    const char *p = cwd;

    strcpy(segs[0], "/");
    strcpy(prefixes[0], "/");
    nseg = 1;
    while (*p && nseg < 16) {
        int n = 0;
        while (*p == '/')
            p++;
        while (*p && *p != '/' && n < 63)
            segs[nseg][n++] = *p++;
        segs[nseg][n] = '\0';
        if (n == 0)
            break;
        join(prefixes[nseg - 1], segs[nseg], prefixes[nseg],
             sizeof(prefixes[nseg]));
        nseg++;
    }

    gui_round_rect(win, r, 6, GUI_SUNKEN);
    gui_round_frame(win, r, 6, GUI_BORDER);
    gui_push_clip(win, gui_mkrc(r.x + 2, r.y + 1, r.w - 4, r.h - 2));

    for (int i = 0; i < nseg; i++) {
        int tw = gui_text_w(segs[i]) + 14;
        gui_rc btn = gui_mkrc(x, r.y + 2, tw, r.h - 4);
        int active = (i == nseg - 1);

        if (gui_button_ex(win, btn, segs[i], ui, 1, active))
            open_dir_abs(prefixes[i]);
        x += tw + 2;
        if (i + 1 < nseg) {
            gui_text(win, x + 2, r.y + (r.h - GUI_FONT_H) / 2, ">",
                     GUI_TEXT_DIM, GUI_TRANSPARENT);
            x += 14;
        }
    }
    gui_pop_clip(win);
}

static int draw_file_list(gui_window *win, gui_rc r, gui_ui *ui)
{
    int bar_w = 12, visible, activated = -1;
    gui_rc rows;

    gui_round_rect(win, r, 7, GUI_SUNKEN);
    gui_round_frame(win, r, 7, GUI_BORDER);

    rows = gui_mkrc(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    visible = rows.h / ROW_H;
    if (visible < 1)
        visible = 1;

    if (nentries > visible) {
        rows.w -= bar_w;
        gui_scrollbar(win, gui_mkrc(r.x + r.w - bar_w - 1, r.y + 1,
                                    bar_w, r.h - 2),
                      nentries, visible, &top_idx, ui);
    } else {
        top_idx = 0;
    }

    if (ui->wheel && gui_hit(r, ui->mx, ui->my)) {
        top_idx -= ui->wheel * 3;
        ui->wheel = 0;
    }
    if (top_idx > nentries - visible)
        top_idx = nentries - visible;
    if (top_idx < 0)
        top_idx = 0;

    if (ui->down && gui_hit(rows, ui->down_x, ui->down_y)) {
        int idx = top_idx + (ui->down_y - rows.y) / ROW_H;
        if (idx >= 0 && idx < nentries) {
            sel_idx = idx;
            if (ui->dbl)
                activated = idx;
        }
    }
    if (ui->dbl && gui_hit(rows, ui->up_x, ui->up_y)) {
        int idx = top_idx + (ui->up_y - rows.y) / ROW_H;
        if (idx >= 0 && idx < nentries)
            activated = idx;
    }

    if (ui->key) {
        int handled = 1;
        switch (ui->key) {
        case KEY_UP:
            if (sel_idx > 0) sel_idx--;
            break;
        case KEY_DOWN:
            if (sel_idx < nentries - 1) sel_idx++;
            break;
        case KEY_PGUP:
            sel_idx -= visible;
            break;
        case KEY_PGDN:
            sel_idx += visible;
            break;
        case KEY_HOME:
            sel_idx = 0;
            break;
        case KEY_END:
            sel_idx = nentries - 1;
            break;
        case '\n':
        case '\r':
            if (sel_idx >= 0 && sel_idx < nentries)
                activated = sel_idx;
            break;
        default:
            handled = 0;
            break;
        }
        if (handled) {
            if (sel_idx < 0) sel_idx = 0;
            if (sel_idx > nentries - 1) sel_idx = nentries - 1;
            ui->key = 0;
        }
    }

    if (sel_idx >= 0) {
        if (sel_idx < top_idx)
            top_idx = sel_idx;
        if (sel_idx >= top_idx + visible)
            top_idx = sel_idx - visible + 1;
        if (top_idx < 0)
            top_idx = 0;
    }

    gui_push_clip(win, rows);
    for (int i = 0; i < visible; i++) {
        int idx = top_idx + i;
        int y = rows.y + i * ROW_H;
        const struct entry *e;
        const char *tag;
        uint32_t badge_col, fg;
        char size_str[24], mode_str[12];
        int show_mode = rows.w >= 480;
        int right_col = rows.x + rows.w - (show_mode ? 176 : 84);

        if (idx >= nentries)
            break;
        e = &entries[idx];
        entry_badge(e, &tag, &badge_col);
        fg = e->is_dir ? GUI_ACCENT : GUI_TEXT;

        if (idx == sel_idx) {
            gui_round_rect(win, gui_mkrc(rows.x + 2, y + 1,
                                         rows.w - 4, ROW_H - 2),
                           5, GUI_ACCENT_DARK);
            fg = GUI_WHITE;
        } else if (idx & 1) {
            gui_rect(win, rows.x, y, rows.w, ROW_H, gui_shade(GUI_SUNKEN, 4));
        }

        gui_badge(win, gui_mkrc(rows.x + 6, y + 4, 44, ROW_H - 8),
                  tag, badge_col);

        gui_push_clip(win, gui_mkrc(rows.x + 58, y, right_col - (rows.x + 64),
                                    ROW_H));
        gui_text(win, rows.x + 58, y + (ROW_H - GUI_FONT_H) / 2, e->name,
                 fg, GUI_TRANSPARENT);
        gui_pop_clip(win);

        if (e->is_dir)
            snprintf(size_str, sizeof(size_str), "<dir>");
        else
            snprintf(size_str, sizeof(size_str), "%u B", e->size);
        gui_text(win, right_col, y + (ROW_H - GUI_FONT_H) / 2, size_str,
                 idx == sel_idx ? GUI_WHITE : GUI_TEXT_DIM, GUI_TRANSPARENT);

        if (show_mode) {
            mode_string(e->mode, mode_str);
            gui_text(win, rows.x + rows.w - 86, y + (ROW_H - GUI_FONT_H) / 2,
                     mode_str, idx == sel_idx ? GUI_WHITE : GUI_TEXT_DIM,
                     GUI_TRANSPARENT);
        }
    }
    gui_pop_clip(win);
    return activated;
}

/* ------------------------------------------------------------ painting */

static void paint(gui_window *win, gui_ui *ui)
{
    static const char *const sort_items[3] = {
        "by name", "by size", "by time"
    };
    int w = win->w, h = win->h;
    int bar_w = w - 20 - 4 * 68;
    int has_banner = (act_mode != ACT_NONE);
    int list_top = has_banner ? 112 : 74;
    int list_h = h - list_top - 68;
    gui_rc list_rc;
    gui_ui list_ui;
    int activated, y, picked;
    int valid_sel = sel_idx >= 0 && sel_idx < nentries;
    int can_mod = valid_sel && strcmp(entries[sel_idx].name, "..") != 0;

    if (bar_w < 80)
        bar_w = 80;
    if (list_h < 60)
        list_h = 60;
    list_rc = gui_mkrc(10, list_top, w - 20, list_h);

    gui_clear(win, GUI_SURFACE);

    /* Top path bar + buttons */
    if (gui_textbox(win, gui_mkrc(10, 8, bar_w, 26), &pathbox, ui))
        open_dir_abs(pathbuf);
    if (gui_button(win, gui_mkrc(w - 10 - 3 * 68 + 4, 8, 64, 26), "Up", ui))
        enter_dir("..");
    if (gui_button(win, gui_mkrc(w - 10 - 2 * 68 + 4, 8, 64, 26), "Home", ui))
        open_dir_abs("/");
    if (gui_button(win, gui_mkrc(w - 10 - 68 + 4, 8, 64, 26), "Reload", ui))
        load_dir();

    /* Clickable breadcrumb bar */
    draw_breadcrumbs(win, gui_mkrc(10, 40, w - 20, 26), ui);

    /* Inline modal banner for New Folder / Rename / Delete */
    if (act_mode == ACT_MKDIR || act_mode == ACT_RENAME) {
        gui_rc ban = gui_mkrc(10, 72, w - 20, 34);
        const char *lbl = act_mode == ACT_MKDIR ? "New folder:" : "Rename to:";
        const char *ok_lbl = act_mode == ACT_MKDIR ? "Create" : "Rename";

        gui_round_rect(win, ban, 6, GUI_PANEL);
        gui_round_frame(win, ban, 6, GUI_ACCENT);
        gui_text(win, ban.x + 8, ban.y + 9, lbl, GUI_TEXT, GUI_TRANSPARENT);
        if (gui_textbox(win, gui_mkrc(ban.x + 106, ban.y + 4,
                                      ban.w - 258, 26), &inputbox, ui)) {
            if (act_mode == ACT_MKDIR)
                do_mkdir();
            else
                do_rename();
        }
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 146, ban.y + 4, 68, 26),
                       ok_lbl, ui)) {
            if (act_mode == ACT_MKDIR)
                do_mkdir();
            else
                do_rename();
        }
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 72, ban.y + 4, 64, 26),
                       "Cancel", ui)) {
            act_mode = ACT_NONE;
            snprintf(status, sizeof(status), "Cancelled");
        }
    } else if (act_mode == ACT_DELETE && valid_sel) {
        gui_rc ban = gui_mkrc(10, 72, w - 20, 34);
        char msg[128];

        gui_round_rect(win, ban, 6, gui_mix(GUI_ERROR, GUI_PANEL, 210));
        gui_round_frame(win, ban, 6, GUI_ERROR);
        snprintf(msg, sizeof(msg), "Delete '%s'?", entries[sel_idx].name);
        gui_text(win, ban.x + 10, ban.y + 9, msg, GUI_TEXT, GUI_TRANSPARENT);
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 178, ban.y + 4, 100, 26),
                       "Delete Yes", ui))
            do_delete();
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 72, ban.y + 4, 64, 26),
                       "Cancel", ui)) {
            act_mode = ACT_NONE;
            snprintf(status, sizeof(status), "Cancelled");
        }
    }

    /* File listing */
    list_ui = *ui;
    if (sortmenu.open || pathbox.focus || inputbox.focus) {
        if (sortmenu.open) {
            list_ui.down = 0;
            list_ui.up = 0;
            list_ui.dbl = 0;
        }
        list_ui.key = 0;
    }
    activated = draw_file_list(win, list_rc, &list_ui);
    if (!sortmenu.open && !pathbox.focus && !inputbox.focus)
        ui->key = list_ui.key;
    if (activated >= 0) {
        open_selection(activated, 0);
    } else if (sel_idx != last_sel) {
        last_sel = sel_idx;
        describe(sel_idx);
    }

    /* Status line */
    y = h - 60;
    gui_rect(win, 10, y, w - 20, GUI_FONT_H + 6, gui_shade(GUI_SURFACE, -10));
    gui_push_clip_xy(win, 12, y, w - 24, GUI_FONT_H + 6);
    gui_text(win, 14, y + 3, status, GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_pop_clip(win);

    /* Action buttons */
    y = h - 34;
    if (gui_button_ex(win, gui_mkrc(10, y, 64, 26), "Open", ui, valid_sel, 0))
        open_selection(sel_idx, 0);
    if (gui_button_ex(win, gui_mkrc(80, y, 64, 26), "Edit", ui,
                      valid_sel && !entries[sel_idx].is_dir, 0))
        open_selection(sel_idx, 1);
    if (gui_button(win, gui_mkrc(150, y, 80, 26), "New Dir", ui)) {
        act_mode = ACT_MKDIR;
        inputbuf[0] = '\0';
        inputbox.len = 0;
        inputbox.caret = 0;
        inputbox.scroll = 0;
        inputbox.focus = 1;
        pathbox.focus = 0;
    }
    if (gui_button_ex(win, gui_mkrc(236, y, 72, 26), "Rename", ui, can_mod, 0)) {
        act_mode = ACT_RENAME;
        snprintf(inputbuf, sizeof(inputbuf), "%s", entries[sel_idx].name);
        inputbox.len = (int)strlen(inputbuf);
        inputbox.caret = inputbox.len;
        inputbox.scroll = 0;
        inputbox.focus = 1;
        pathbox.focus = 0;
    }
    if (gui_button(win, gui_mkrc(314, y, 88, 26), "Copy Path", ui))
        copy_path_to_talon();
    if (gui_button_ex(win, gui_mkrc(408, y, 72, 26), "Delete", ui, can_mod, 0)) {
        act_mode = ACT_DELETE;
        snprintf(status, sizeof(status), "Confirm delete %s",
                 entries[sel_idx].name);
    }

    if (w >= 610 && gui_checkbox(win, w - 116, y + 5, "dirs first",
                                 &dirs_first, ui))
        sort_entries();

    /* Drawn last so the open drop-down covers the listing. */
    picked = gui_menu(win, gui_mkrc(w - 10 - 4 * 68 + 4, 8, 64, 26),
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
    pathbox.buf = pathbuf;
    pathbox.cap = sizeof(pathbuf);
    inputbox.buf = inputbuf;
    inputbox.cap = sizeof(inputbuf);

    const char *cwd_opt = kestrel_startup_cwd();
    const char *dir_opt = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strncmp(arg, "--cwd=", 6) == 0)
            cwd_opt = arg + 6;
        else if (arg[0] != '\0' && !dir_opt)
            dir_opt = arg;
    }
    snprintf(cwd, sizeof(cwd), "%s", cwd_opt[0] ? cwd_opt : "/");
    normalize(cwd);
    if (dir_opt) {
        char tmp[MAX_PATH];
        join(cwd, dir_opt, tmp, sizeof(tmp));
        snprintf(cwd, sizeof(cwd), "%s", tmp);
        normalize(cwd);
    }
    if (stat_(cwd, &st) != 0 || !st.is_dir)
        strcpy(cwd, "/");

    win = gui_open("Files", 120, 80, INIT_W, INIT_H, 0);
    if (!win) {
        printf("gfiles: no window manager available\n");
        return 1;
    }

    load_dir();

    while (win->open) {
        int got = gui_next_event(win, &ev, 60);

        reap_children();
        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == 27 && act_mode != ACT_NONE) {
                act_mode = ACT_NONE;
                inputbox.focus = 0;
                snprintf(status, sizeof(status), "Cancelled");
            }
            dirty = 1;
        } else if (pathbox.focus || inputbox.focus) {
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
