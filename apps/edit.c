/* edit.c - KestrelOS text & code editor.
 *
 * Uses libgui's syntax-aware gui_editor widget in a resizable desktop window
 * (line numbers, C/JS/HTML/CSS/SH/@krec highlighting, click/drag/double-click
 * selection, mouse wheel + scrollbar, Talon clipboard copy/cut/paste, Ctrl+S
 * save, Open / Save As toolbar, dirty indicator, and KEV_RESIZE reflow).
 *
 * Falls back to the full-screen ANSI console editor when invoked with -t or
 * when no window manager is available.
 *
 * Usage: edit [-t] [--cwd=<dir>] [file]
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define ED_MAX_PATH  256
#define ED_MAX_FILE  (4UL * 1024UL * 1024UL)

/* ----------------------------------------------------------- path helpers */

static void normalize_path(char *path)
{
    char out[ED_MAX_PATH];
    char seg[ED_MAX_PATH];
    const char *p = path;
    int len = 1;

    out[0] = '/';
    out[1] = '\0';
    while (*p) {
        int n = 0;

        while (*p == '/')
            p++;
        while (*p && *p != '/' && n < (int)sizeof(seg) - 1)
            seg[n++] = *p++;
        seg[n] = '\0';
        if (n == 0)
            break;
        if (strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            while (len > 1 && out[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            out[len] = '\0';
            continue;
        }
        if (len > 1 && len < (int)sizeof(out) - 1)
            out[len++] = '/';
        for (int i = 0; seg[i] && len < (int)sizeof(out) - 1; i++)
            out[len++] = seg[i];
        out[len] = '\0';
    }
    strncpy(path, out, ED_MAX_PATH - 1);
    path[ED_MAX_PATH - 1] = '\0';
}

static void resolve_path(const char *path, const char *cwd, char *out,
                         unsigned long outsz)
{
    if (!path || !path[0]) {
        out[0] = '\0';
        return;
    }
    if (path[0] == '/' || !cwd || !cwd[0])
        snprintf(out, outsz, "%s", path);
    else if (strcmp(cwd, "/") == 0)
        snprintf(out, outsz, "/%s", path);
    else
        snprintf(out, outsz, "%s/%s", cwd, path);
    normalize_path(out);
}

/* ------------------------------------------------------ GUI editor mode */

enum confirm_kind {
    CONF_NONE = 0,
    CONF_QUIT,
    CONF_OPEN,
    CONF_NEW
};

static const char *syntax_label(int syn)
{
    switch (syn) {
    case GUI_SYNTAX_C:    return "C";
    case GUI_SYNTAX_JS:   return "JS";
    case GUI_SYNTAX_HTML: return "HTML";
    case GUI_SYNTAX_CSS:  return "CSS";
    case GUI_SYNTAX_SH:   return "SH";
    case GUI_SYNTAX_KREC: return "KREC";
    default:              return "TXT";
    }
}

static int detect_syntax(const char *path, const char *text)
{
    int syn = gui_editor_syntax_for_path(path);

    if (syn != GUI_SYNTAX_NONE)
        return syn;
    if (text) {
        if (strncmp(text, "@krec ", 6) == 0 ||
            strncmp(text, "@krec\t", 6) == 0)
            return GUI_SYNTAX_KREC;
        if (strncmp(text, "#!", 2) == 0)
            return GUI_SYNTAX_SH;
    }
    return GUI_SYNTAX_NONE;
}

static int read_entire_file(const char *path, char **out_buf, int *out_err)
{
    struct k_stat st;
    unsigned long cap = 4096, len = 0;
    char *buf;
    int fd;

    *out_buf = 0;
    if (stat_(path, &st) != 0) {
        *out_err = 1; /* missing / new file */
        return -1;
    }
    if (st.is_dir) {
        *out_err = 2; /* is a directory */
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        *out_err = 3;
        return -1;
    }
    buf = malloc(cap);
    if (!buf) {
        close(fd);
        *out_err = 4;
        return -1;
    }
    for (;;) {
        if (len + 1024 + 1 > cap) {
            unsigned long ncap = cap * 2;
            char *nbuf;
            if (ncap > ED_MAX_FILE + 1)
                ncap = ED_MAX_FILE + 1;
            if (ncap <= cap) {
                free(buf);
                close(fd);
                *out_err = 4;
                return -1;
            }
            nbuf = realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                close(fd);
                *out_err = 4;
                return -1;
            }
            buf = nbuf;
            cap = ncap;
        }
        {
            long n = read(fd, buf + len, cap - len - 1);
            if (n < 0) {
                free(buf);
                close(fd);
                *out_err = 3;
                return -1;
            }
            if (n == 0)
                break;
            len += (unsigned long)n;
        }
    }
    close(fd);
    buf[len] = '\0';
    *out_buf = buf;
    *out_err = 0;
    return 0;
}

struct gui_ed_app {
    gui_editor ed;
    char cwd[ED_MAX_PATH];
    char filepath[ED_MAX_PATH];
    char pathbuf[ED_MAX_PATH];
    char pending_path[ED_MAX_PATH];
    char status[160];
    gui_textbox_state pathbox;
    gui_menu_state syn_menu;
    int syntax_auto;
    int confirm;
    int want_quit;
    int pathbox_for_save;
};

static void sync_pathbox(struct gui_ed_app *app, const char *p)
{
    snprintf(app->pathbuf, sizeof(app->pathbuf), "%s", p ? p : "");
    app->pathbox.len = (int)strlen(app->pathbuf);
    app->pathbox.caret = app->pathbox.len;
    app->pathbox.scroll = 0;
}

static void gui_load_path(struct gui_ed_app *app, const char *raw)
{
    char path[ED_MAX_PATH];
    char *buf = 0;
    int err = 0;

    resolve_path(raw, app->cwd, path, sizeof(path));
    if (!path[0]) {
        snprintf(app->status, sizeof(app->status), "Enter a file path first");
        return;
    }
    if (read_entire_file(path, &buf, &err) == 0) {
        if (gui_editor_set_text(&app->ed, buf) != 0) {
            free(buf);
            snprintf(app->status, sizeof(app->status), "Out of memory");
            return;
        }
        snprintf(app->filepath, sizeof(app->filepath), "%s", path);
        sync_pathbox(app, path);
        if (app->syntax_auto)
            gui_editor_set_syntax(&app->ed, detect_syntax(path, buf));
        free(buf);
        snprintf(app->status, sizeof(app->status), "Loaded %s (%d lines)",
                 path, app->ed.nlines);
    } else if (err == 2) {
        snprintf(app->status, sizeof(app->status), "%s is a directory", path);
    } else if (err == 1) {
        gui_editor_set_text(&app->ed, "");
        snprintf(app->filepath, sizeof(app->filepath), "%s", path);
        sync_pathbox(app, path);
        if (app->syntax_auto)
            gui_editor_set_syntax(&app->ed,
                                  gui_editor_syntax_for_path(path));
        snprintf(app->status, sizeof(app->status), "New file: %s", path);
    } else {
        snprintf(app->status, sizeof(app->status), "Cannot read %s", path);
    }
    app->confirm = CONF_NONE;
    app->ed.focus = 1;
    app->pathbox.focus = 0;
}

static int gui_save_to(struct gui_ed_app *app, const char *raw)
{
    char path[ED_MAX_PATH];
    struct k_stat st;
    int total, fd;
    char *buf;
    unsigned long off = 0, want;

    resolve_path(raw, app->cwd, path, sizeof(path));
    if (!path[0]) {
        snprintf(app->status, sizeof(app->status),
                 "Enter a file path in the bar to Save As");
        app->pathbox_for_save = 1;
        app->pathbox.focus = 1;
        app->ed.focus = 0;
        return -1;
    }
    if (stat_(path, &st) == 0 && st.is_dir) {
        snprintf(app->status, sizeof(app->status),
                 "Save failed: %s is a directory", path);
        return -1;
    }
    total = gui_editor_get_text(&app->ed, 0, 0);
    buf = malloc((unsigned long)total + 2);
    if (!buf) {
        snprintf(app->status, sizeof(app->status), "Save failed: out of memory");
        return -1;
    }
    gui_editor_get_text(&app->ed, buf, total + 1);
    want = (unsigned long)total;
    if (want > 0 && buf[want - 1] != '\n')
        buf[want++] = '\n';

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(buf);
        snprintf(app->status, sizeof(app->status), "Save failed: cannot open %s",
                 path);
        return -1;
    }
    while (off < want) {
        long w = write(fd, buf + off, want - off);
        if (w <= 0) {
            close(fd);
            free(buf);
            snprintf(app->status, sizeof(app->status), "Save failed: write error");
            return -1;
        }
        off += (unsigned long)w;
    }
    close(fd);
    if (app->syntax_auto)
        gui_editor_set_syntax(&app->ed, detect_syntax(path, buf));
    free(buf);

    snprintf(app->filepath, sizeof(app->filepath), "%s", path);
    sync_pathbox(app, path);
    app->ed.modified = 0;
    app->confirm = CONF_NONE;
    app->ed.focus = 1;
    app->pathbox.focus = 0;
    snprintf(app->status, sizeof(app->status), "Saved %s (%lu bytes, %d lines)",
             path, want, app->ed.nlines);
    return 0;
}

static void gui_reset_new(struct gui_ed_app *app)
{
    gui_editor_set_text(&app->ed, "");
    app->filepath[0] = '\0';
    sync_pathbox(app, "");
    if (app->syntax_auto)
        gui_editor_set_syntax(&app->ed, GUI_SYNTAX_NONE);
    app->confirm = CONF_NONE;
    app->ed.focus = 1;
    app->pathbox.focus = 0;
    snprintf(app->status, sizeof(app->status), "New untitled buffer");
}

static void trigger_open(struct gui_ed_app *app, const char *raw)
{
    if (!raw || !raw[0]) {
        snprintf(app->status, sizeof(app->status), "Enter a path to open");
        app->pathbox.focus = 1;
        app->ed.focus = 0;
        return;
    }
    if (app->ed.modified) {
        snprintf(app->pending_path, sizeof(app->pending_path), "%s", raw);
        app->confirm = CONF_OPEN;
        snprintf(app->status, sizeof(app->status),
                 "Unsaved changes: Save or Discard before opening?");
        return;
    }
    gui_load_path(app, raw);
}

static void trigger_new(struct gui_ed_app *app)
{
    if (app->ed.modified) {
        app->confirm = CONF_NEW;
        snprintf(app->status, sizeof(app->status),
                 "Unsaved changes: Save or Discard before New?");
        return;
    }
    gui_reset_new(app);
}

static gui_rc editor_rect(const gui_window *win, int has_confirm)
{
    int top_h = has_confirm ? 84 : 46;
    int status_h = 26;
    int h = win->h - top_h - status_h - 6;

    if (h < 40)
        h = 40;
    return gui_mkrc(8, top_h + 2, win->w > 24 ? win->w - 16 : 8, h);
}

static void synth_editor_key(struct gui_ed_app *app, gui_rc ed_rc, int key)
{
    gui_ui sui;

    memset(&sui, 0, sizeof(sui));
    sui.key = key;
    app->ed.focus = 1;
    app->pathbox.focus = 0;
    gui_editor_event(&app->ed, ed_rc, &sui);
}

static void paint_gui(gui_window *win, gui_ui *ui, struct gui_ed_app *app)
{
    static const char *const syn_items[8] = {
        "Auto", "Plain", "C", "JS", "HTML", "CSS", "Shell", "KREC"
    };
    int w = win->w, h = win->h;
    int top_h = 46;
    int status_h = 26;
    gui_rc ed_rc;
    int btn_w = 56, save_as_w = 68, syn_w = 62, copy_w = 50, paste_w = 56;
    int right_used = btn_w + save_as_w + copy_w + paste_w + syn_w + 5 * 6 + 8;
    int path_x = 68;
    int path_w = w - path_x - right_used;
    int syn_x = w - syn_w - 8;
    int x;
    int picked;
    gui_ui ed_ui;

    if (path_w < 90)
        path_w = 90;

    gui_clear(win, GUI_SURFACE);

    /* Toolbar */
    gui_rect(win, 0, 0, w, top_h, GUI_PANEL);
    gui_rect(win, 0, top_h - 1, w, 1, GUI_BORDER);

    if (gui_button(win, gui_mkrc(8, 10, 48, 26), "New", ui)) {
        app->pathbox_for_save = 0;
        trigger_new(app);
    }

    {
        gui_rc prc = gui_mkrc(path_x, 10, path_w, 26);
        if (ui->down && gui_hit(prc, ui->down_x, ui->down_y))
            app->pathbox_for_save = 0;
        if (gui_textbox(win, prc, &app->pathbox, ui)) {
            if (app->pathbox_for_save) {
                app->pathbox_for_save = 0;
                gui_save_to(app, app->pathbuf);
            } else {
                trigger_open(app, app->pathbuf);
            }
        }
    }

    x = path_x + path_w + 6;
    if (gui_button(win, gui_mkrc(x, 10, btn_w, 26), "Open", ui)) {
        app->pathbox_for_save = 0;
        trigger_open(app, app->pathbuf);
    }
    x += btn_w + 6;

    if (gui_button_ex(win, gui_mkrc(x, 10, btn_w, 26), "Save", ui,
                      1, app->ed.modified)) {
        const char *target = app->filepath[0] ? app->filepath : app->pathbuf;
        app->pathbox_for_save = 0;
        gui_save_to(app, target);
    }
    x += btn_w + 6;

    if (gui_button(win, gui_mkrc(x, 10, save_as_w, 26), "SaveAs", ui)) {
        app->pathbox_for_save = 0;
        gui_save_to(app, app->pathbuf);
    }
    x += save_as_w + 6;

    ed_rc = editor_rect(win, app->confirm != CONF_NONE);

    if (gui_button(win, gui_mkrc(x, 10, copy_w, 26), "Copy", ui)) {
        synth_editor_key(app, ed_rc, 3);
        snprintf(app->status, sizeof(app->status), "Copied selection to Talon");
    }
    x += copy_w + 6;

    if (gui_button(win, gui_mkrc(x, 10, paste_w, 26), "Paste", ui)) {
        synth_editor_key(app, ed_rc, 22);
        snprintf(app->status, sizeof(app->status), "Pasted from Talon");
    }

    /* Optional confirmation banner */
    if (app->confirm != CONF_NONE) {
        gui_rc ban = gui_mkrc(8, 48, w - 16, 32);
        const char *msg = app->confirm == CONF_QUIT
                              ? "Unsaved changes before closing:"
                              : "Unsaved changes in current file:";
        gui_round_rect(win, ban, 6, gui_mix(GUI_WARN, GUI_PANEL, 210));
        gui_round_frame(win, ban, 6, GUI_WARN);
        gui_text(win, ban.x + 10, ban.y + 8, msg, GUI_TEXT, GUI_TRANSPARENT);

        if (gui_button(win, gui_mkrc(ban.x + ban.w - 236, ban.y + 3, 74, 26),
                       "Save", ui)) {
            const char *t = app->filepath[0] ? app->filepath : app->pathbuf;
            int prev = app->confirm;
            if (gui_save_to(app, t) == 0) {
                if (prev == CONF_QUIT)
                    app->want_quit = 1;
                else if (prev == CONF_OPEN)
                    gui_load_path(app, app->pending_path);
                else if (prev == CONF_NEW)
                    gui_reset_new(app);
            }
        }
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 156, ban.y + 3, 76, 26),
                       "Discard", ui)) {
            int prev = app->confirm;
            if (prev == CONF_QUIT)
                app->want_quit = 1;
            else if (prev == CONF_OPEN)
                gui_load_path(app, app->pending_path);
            else if (prev == CONF_NEW)
                gui_reset_new(app);
        }
        if (gui_button(win, gui_mkrc(ban.x + ban.w - 74, ban.y + 3, 66, 26),
                       "Cancel", ui)) {
            app->confirm = CONF_NONE;
            app->ed.focus = 1;
            snprintf(app->status, sizeof(app->status), "Cancelled");
        }
    }

    /* Editor widget (mask clicks while syntax dropdown is open) */
    ed_rc = editor_rect(win, app->confirm != CONF_NONE);
    ed_ui = *ui;
    if (app->syn_menu.open) {
        ed_ui.down = 0;
        ed_ui.up = 0;
        ed_ui.dbl = 0;
        ed_ui.key = 0;
    }
    if (!app->pathbox.focus && ed_ui.key) {
        int k = ed_ui.key;
        if (k == 19) { /* Ctrl+S */
            const char *t = app->filepath[0] ? app->filepath : app->pathbuf;
            gui_save_to(app, t);
            ed_ui.key = 0;
            ui->key = 0;
        } else if (k == 15) { /* Ctrl+O */
            app->pathbox_for_save = 0;
            app->pathbox.focus = 1;
            app->ed.focus = 0;
            ed_ui.key = 0;
            ui->key = 0;
        } else if (k == 17) { /* Ctrl+Q */
            if (!app->ed.modified || app->confirm == CONF_QUIT)
                app->want_quit = 1;
            else
                app->confirm = CONF_QUIT;
            ed_ui.key = 0;
            ui->key = 0;
        } else if (k == 14) { /* Ctrl+N */
            trigger_new(app);
            ed_ui.key = 0;
            ui->key = 0;
        }
    }
    gui_editor_event(&app->ed, ed_rc, &ed_ui);
    if (!app->syn_menu.open && !ed_ui.key)
        ui->key = 0;
    gui_editor_draw(win, ed_rc, &app->ed, &ed_ui);

    /* Status bar */
    {
        int sy = h - status_h;
        char right[96];
        char title[ED_MAX_PATH + 16];
        const char *base = app->filepath[0] ? app->filepath : "(untitled)";
        int right_w;

        gui_rect(win, 0, sy, w, status_h, GUI_PANEL);
        gui_rect(win, 0, sy, w, 1, GUI_BORDER);

        gui_badge(win, gui_mkrc(8, sy + 4, 24, 18),
                  app->ed.modified ? "*" : "=",
                  app->ed.modified ? GUI_WARN : GUI_OK);

        snprintf(right, sizeof(right), "%s  Ln %d/%d Col %d",
                 syntax_label(app->ed.syntax),
                 app->ed.cursor_line + 1, app->ed.nlines,
                 app->ed.cursor_col + 1);
        right_w = gui_text_w(right);

        snprintf(title, sizeof(title), "%s%s  -  %s", base,
                 app->ed.modified ? " *" : "", app->status);
        gui_push_clip_xy(win, 38, sy, w - right_w - 56, status_h);
        gui_text(win, 38, sy + (status_h - GUI_FONT_H) / 2, title,
                 GUI_TEXT_DIM, GUI_TRANSPARENT);
        gui_pop_clip(win);

        gui_text(win, w - right_w - 10, sy + (status_h - GUI_FONT_H) / 2,
                 right, GUI_TEXT, GUI_TRANSPARENT);
    }

    /* Syntax dropdown drawn last to overlay editor */
    picked = gui_menu(win, gui_mkrc(syn_x, 10, syn_w, 26),
                      syntax_label(app->ed.syntax), syn_items, 8,
                      &app->syn_menu, ui);
    if (picked >= 0) {
        if (picked == 0) {
            app->syntax_auto = 1;
            gui_editor_set_syntax(
                &app->ed,
                app->filepath[0] ? gui_editor_syntax_for_path(app->filepath)
                                 : GUI_SYNTAX_NONE);
        } else {
            app->syntax_auto = 0;
            gui_editor_set_syntax(&app->ed, picked - 1);
        }
        app->ed.focus = 1;
        app->pathbox.focus = 0;
    }
}

static int run_gui_editor(const char *cwd, const char *file_arg)
{
    gui_window *win;
    struct gui_ed_app *app;
    gui_ui ui;
    struct k_event ev;
    int dirty = 1;
    unsigned long last_blink;

    win = gui_open("Editor", 90, 60, 760, 540, 0);
    if (!win)
        return -1;

    app = calloc(1, sizeof(*app));
    if (!app) {
        gui_close(win);
        printf("edit: out of memory\n");
        return 1;
    }

    gui_editor_init(&app->ed);
    snprintf(app->cwd, sizeof(app->cwd), "%s", cwd && cwd[0] ? cwd : "/");
    normalize_path(app->cwd);
    app->pathbox.buf = app->pathbuf;
    app->pathbox.cap = sizeof(app->pathbuf);
    app->syntax_auto = 1;

    if (file_arg && file_arg[0])
        gui_load_path(app, file_arg);
    else
        gui_reset_new(app);

    memset(&ui, 0, sizeof(ui));
    last_blink = uptime_ms() / 500;

    while (win->open && !app->want_quit) {
        int got = gui_next_event(win, &ev, 80);
        unsigned long blink = uptime_ms() / 500;

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            dirty = 1;
        } else if (blink != last_blink && (app->ed.focus || app->pathbox.focus)) {
            dirty = 1;
        }
        last_blink = blink;
        if (!dirty)
            continue;
        paint_gui(win, &ui, app);
        if (app->want_quit)
            break;
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_editor_destroy(&app->ed);
    free(app);
    gui_close(win);
    return 0;
}

/* ----------------------------------------------------- console fallback */

#define TUI_MAX_LINES 2000
#define TUI_MAX_COLS  256
#define TUI_TEXT_ROWS 23
#define TUI_TEXT_COLS 80

static char *tui_lines[TUI_MAX_LINES];
static int tui_nlines = 0;
static char tui_filename[128];
static int tui_modified = 0;
static int tui_cy = 0, tui_cx = 0, tui_top = 0, tui_left = 0;
static char tui_msg[80];
static int tui_quit_pending = 0;
static int tui_save_pending = 0;
static int tui_truncated = 0;
static int tui_had_nl = 1;

static char *tui_alloc_line(const char *text)
{
    char *l = malloc(TUI_MAX_COLS);

    if (!l) {
        printf("edit: out of memory\n");
        exit(1);
    }
    l[0] = 0;
    if (text)
        strncpy(l, text, TUI_MAX_COLS - 1);
    l[TUI_MAX_COLS - 1] = 0;
    return l;
}

static void tui_set_msg(const char *m)
{
    strncpy(tui_msg, m, sizeof(tui_msg) - 1);
    tui_msg[sizeof(tui_msg) - 1] = 0;
}

static void tui_load(void)
{
    struct k_stat st;
    int fd;
    char cur[TUI_MAX_COLS], buf[512];
    int curlen = 0, last = -1;
    long n;

    if (stat_(tui_filename, &st) == 0 && st.is_dir) {
        printf("edit: %s: is a directory\n", tui_filename);
        exit(1);
    }
    fd = open(tui_filename, O_RDONLY);
    if (fd < 0) {
        tui_nlines = 1;
        tui_lines[0] = tui_alloc_line(0);
        tui_set_msg("new file");
        return;
    }
    tui_nlines = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            last = (unsigned char)c;
            if (c == '\n') {
                if (tui_nlines < TUI_MAX_LINES) {
                    cur[curlen] = 0;
                    tui_lines[tui_nlines++] = tui_alloc_line(cur);
                } else {
                    tui_truncated = 1;
                }
                curlen = 0;
            } else if (c != '\r') {
                if (curlen < TUI_MAX_COLS - 1)
                    cur[curlen++] = c;
                else
                    tui_truncated = 1;
            }
        }
    }
    tui_had_nl = (last < 0 || last == '\n');
    if (curlen > 0 || tui_nlines == 0) {
        if (tui_nlines < TUI_MAX_LINES) {
            cur[curlen] = 0;
            tui_lines[tui_nlines++] = tui_alloc_line(cur);
        } else {
            tui_truncated = 1;
        }
    }
    close(fd);
    tui_set_msg(tui_truncated ? "loaded, TRUNCATED - saving will destroy data"
                              : "loaded");
}

static void tui_save(void)
{
    int fd;
    char m[64];

    if (tui_truncated && !tui_save_pending) {
        tui_save_pending = 1;
        tui_set_msg("buffer is TRUNCATED - ctrl-S again to overwrite anyway");
        return;
    }
    tui_save_pending = 0;
    fd = open(tui_filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        tui_set_msg("save failed: cannot open file");
        return;
    }
    for (int i = 0; i < tui_nlines; i++) {
        unsigned long len = strlen(tui_lines[i]);
        if (len > 0)
            write(fd, tui_lines[i], len);
        if (i < tui_nlines - 1 || (tui_had_nl && (tui_nlines > 1 || len > 0)))
            write(fd, "\n", 1);
    }
    close(fd);
    tui_modified = 0;
    snprintf(m, sizeof(m), "saved %d lines", tui_nlines);
    tui_set_msg(m);
}

static void tui_draw(void)
{
    char status[96];
    int slen;

    term_hide_cursor();
    for (int row = 0; row < TUI_TEXT_ROWS; row++) {
        int li = tui_top + row;
        term_goto(row + 1, 1);
        printf("\033[K");
        if (li < tui_nlines) {
            const char *l = tui_lines[li];
            unsigned long len = strlen(l);
            if ((unsigned long)tui_left < len) {
                const char *p = l + tui_left;
                unsigned long vis = len - (unsigned long)tui_left;
                char vbuf[TUI_TEXT_COLS];
                if (vis > TUI_TEXT_COLS)
                    vis = TUI_TEXT_COLS;
                for (unsigned long k = 0; k < vis; k++) {
                    unsigned char ch = (unsigned char)p[k];
                    vbuf[k] = (ch >= 32 && ch < 127) ? (char)ch : '?';
                }
                write(1, vbuf, vis);
            }
        } else {
            term_color(TERM_CYAN);
            printf("~");
            term_reset();
        }
    }
    snprintf(status, sizeof(status),
             " %s%s%s  ln %d/%d col %d  ^S save ^Q quit",
             tui_filename[0] ? tui_filename : "(no name)",
             tui_modified ? " *" : "",
             tui_truncated ? " [TRUNCATED]" : "",
             tui_cy + 1, tui_nlines, tui_cx + 1);
    term_goto(24, 1);
    printf("\033[7m");
    slen = (int)strlen(status);
    write(1, status, slen > TUI_TEXT_COLS ? TUI_TEXT_COLS : (unsigned long)slen);
    for (int i = slen; i < TUI_TEXT_COLS; i++)
        putchar(' ');
    term_reset();
    term_goto(25, 1);
    printf("\033[K%s", tui_msg);
    term_goto(tui_cy - tui_top + 1, tui_cx - tui_left + 1);
    term_show_cursor();
}

static void tui_clamp_cx(void)
{
    int len = (int)strlen(tui_lines[tui_cy]);
    if (tui_cx > len)
        tui_cx = len;
}

static void tui_delete_at(void)
{
    char *l = tui_lines[tui_cy];
    int len = (int)strlen(l);

    if (tui_cx < len) {
        memmove(l + tui_cx, l + tui_cx + 1, (unsigned long)(len - tui_cx));
        tui_modified = 1;
    } else if (tui_cy < tui_nlines - 1) {
        char *next = tui_lines[tui_cy + 1];
        int nl = (int)strlen(next);
        if (nl > TUI_MAX_COLS - 1 - len) {
            tui_set_msg("lines too long to join");
            return;
        }
        memcpy(l + len, next, (unsigned long)nl);
        l[len + nl] = 0;
        free(next);
        for (int i = tui_cy + 1; i < tui_nlines - 1; i++)
            tui_lines[i] = tui_lines[i + 1];
        tui_nlines--;
        tui_modified = 1;
    }
}

static int run_tui_editor(const char *cwd, const char *arg)
{
    if (arg) {
        resolve_path(arg, cwd, tui_filename, sizeof(tui_filename));
        tui_load();
    } else {
        tui_filename[0] = '\0';
        tui_nlines = 1;
        tui_lines[0] = tui_alloc_line(0);
        tui_set_msg("empty buffer (no filename: save will fail)");
    }

    term_clear();
    for (;;) {
        int c;
        if (tui_cy < tui_top) tui_top = tui_cy;
        if (tui_cy >= tui_top + TUI_TEXT_ROWS) tui_top = tui_cy - TUI_TEXT_ROWS + 1;
        if (tui_cx < tui_left) tui_left = tui_cx;
        if (tui_cx >= tui_left + TUI_TEXT_COLS) tui_left = tui_cx - TUI_TEXT_COLS + 1;
        tui_draw();
        c = getchar();
        if (c == EOF)
            break;
        if (c != 17) tui_quit_pending = 0;
        if (c != 19) tui_save_pending = 0;
        switch (c) {
        case KEY_UP:
            if (tui_cy > 0) tui_cy--;
            tui_clamp_cx();
            break;
        case KEY_DOWN:
            if (tui_cy < tui_nlines - 1) tui_cy++;
            tui_clamp_cx();
            break;
        case KEY_LEFT:
            if (tui_cx > 0) tui_cx--;
            else if (tui_cy > 0) { tui_cy--; tui_cx = (int)strlen(tui_lines[tui_cy]); }
            break;
        case KEY_RIGHT:
            if (tui_cx < (int)strlen(tui_lines[tui_cy])) tui_cx++;
            else if (tui_cy < tui_nlines - 1) { tui_cy++; tui_cx = 0; }
            break;
        case KEY_HOME: tui_cx = 0; break;
        case KEY_END: tui_cx = (int)strlen(tui_lines[tui_cy]); break;
        case KEY_PGUP:
            tui_cy -= TUI_TEXT_ROWS;
            if (tui_cy < 0) tui_cy = 0;
            tui_clamp_cx();
            break;
        case KEY_PGDN:
            tui_cy += TUI_TEXT_ROWS;
            if (tui_cy > tui_nlines - 1) tui_cy = tui_nlines - 1;
            tui_clamp_cx();
            break;
        case KEY_DELETE:
            tui_delete_at();
            break;
        case 8:
        case 127:
            if (tui_cx > 0) { tui_cx--; tui_delete_at(); }
            else if (tui_cy > 0) {
                tui_cy--;
                tui_cx = (int)strlen(tui_lines[tui_cy]);
                tui_delete_at();
            }
            break;
        case '\n':
        case '\r':
            if (tui_nlines >= TUI_MAX_LINES) {
                tui_set_msg("buffer full");
            } else {
                char *l = tui_lines[tui_cy];
                int len = (int)strlen(l);
                if (tui_cx > len) tui_cx = len;
                for (int i = tui_nlines; i > tui_cy + 1; i--)
                    tui_lines[i] = tui_lines[i - 1];
                tui_lines[tui_cy + 1] = tui_alloc_line(l + tui_cx);
                l[tui_cx] = 0;
                tui_nlines++;
                tui_cy++;
                tui_cx = 0;
                tui_modified = 1;
            }
            break;
        case 19:
            if (tui_filename[0]) tui_save();
            else tui_set_msg("no filename to save to");
            break;
        case 17:
            if (tui_modified && !tui_quit_pending) {
                tui_quit_pending = 1;
                tui_set_msg("unsaved changes! ctrl-Q again to quit");
            } else {
                term_clear();
                term_show_cursor();
                return 0;
            }
            break;
        default:
            if (c >= 32 && c < 127) {
                char *l = tui_lines[tui_cy];
                int len = (int)strlen(l);
                if (len >= TUI_MAX_COLS - 1) {
                    tui_set_msg("line full");
                } else {
                    if (tui_cx > len) tui_cx = len;
                    memmove(l + tui_cx + 1, l + tui_cx,
                            (unsigned long)(len - tui_cx + 1));
                    l[tui_cx++] = (char)c;
                    tui_modified = 1;
                }
            }
            break;
        }
    }
    term_clear();
    term_show_cursor();
    return 0;
}

int main(int argc, char *argv[])
{
    const char *cwd = "/";
    const char *arg = 0;
    int force_text = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            cwd = argv[i] + 6;
        else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--text") == 0)
            force_text = 1;
        else if (!arg)
            arg = argv[i];
    }

    if (!force_text) {
        int rc = run_gui_editor(cwd, arg);
        if (rc >= 0)
            return rc;
    }
    return run_tui_editor(cwd, arg);
}
