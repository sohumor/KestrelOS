/* libgui: multi-line syntax-aware text/code editor widget.
 *
 * Provides dynamic line storage, line numbers, scrolling (wheel + scrollbar),
 * mouse click/drag/double-click selection, keyboard navigation, Talon
 * clipboard copy/cut/paste, and syntax coloring for C, JS, HTML, CSS, SH,
 * and KREC.
 */

#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_LINE_INIT_CAP 32
#define ED_ARR_INIT_CAP  16
#define ED_BAR_W         12
#define CARET_BLINK_MS   500

/* ---------------------------------------------------------- char helpers */

static int ch_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int ch_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int ch_alnum(int c)
{
    return ch_alpha(c) || ch_digit(c);
}

static int ch_hex(int c)
{
    return ch_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int ch_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int streq_nocase(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (ch_lower((unsigned char)*a) != ch_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ----------------------------------------------------------- line buffer */

static int line_reserve(gui_editor_line *ln, int need_cap)
{
    int ncap;
    char *nbuf;

    if (need_cap <= ln->cap)
        return 0;
    ncap = ln->cap > 0 ? ln->cap : ED_LINE_INIT_CAP;
    while (ncap < need_cap) {
        if (ncap > (1 << 28))
            return -1;
        ncap *= 2;
    }
    nbuf = (char *)realloc(ln->text, (unsigned long)ncap);
    if (!nbuf)
        return -1;
    ln->text = nbuf;
    ln->cap = ncap;
    return 0;
}

static int line_init(gui_editor_line *ln, const char *s, int len)
{
    ln->text = 0;
    ln->len = 0;
    ln->cap = 0;
    if (len < 0)
        len = s ? (int)strlen(s) : 0;
    if (line_reserve(ln, len + 1) != 0)
        return -1;
    if (s && len > 0)
        memcpy(ln->text, s, (unsigned long)len);
    ln->text[len] = '\0';
    ln->len = len;
    return 0;
}

static int lines_reserve(gui_editor *ed, int need)
{
    int ncap;
    gui_editor_line *narr;

    if (need <= ed->lines_cap)
        return 0;
    ncap = ed->lines_cap > 0 ? ed->lines_cap : ED_ARR_INIT_CAP;
    while (ncap < need) {
        if (ncap > (1 << 24))
            return -1;
        ncap *= 2;
    }
    narr = (gui_editor_line *)realloc(
        ed->lines, (unsigned long)ncap * sizeof(gui_editor_line));
    if (!narr)
        return -1;
    ed->lines = narr;
    ed->lines_cap = ncap;
    return 0;
}

static void free_all_lines(gui_editor *ed)
{
    int i;

    if (ed->lines) {
        for (i = 0; i < ed->nlines; i++)
            free(ed->lines[i].text);
        free(ed->lines);
    }
    ed->lines = 0;
    ed->nlines = 0;
    ed->lines_cap = 0;
}

static int insert_new_line(gui_editor *ed, int at, const char *s, int len)
{
    int i;

    if (at < 0 || at > ed->nlines)
        return -1;
    if (lines_reserve(ed, ed->nlines + 1) != 0)
        return -1;
    for (i = ed->nlines; i > at; i--)
        ed->lines[i] = ed->lines[i - 1];
    if (line_init(&ed->lines[at], s, len) != 0) {
        for (i = at; i < ed->nlines; i++)
            ed->lines[i] = ed->lines[i + 1];
        return -1;
    }
    ed->nlines++;
    return 0;
}

static void remove_line(gui_editor *ed, int at)
{
    int i;

    if (at < 0 || at >= ed->nlines || ed->nlines <= 1)
        return;
    free(ed->lines[at].text);
    for (i = at; i < ed->nlines - 1; i++)
        ed->lines[i] = ed->lines[i + 1];
    ed->nlines--;
}

static void clamp_cursor(gui_editor *ed)
{
    if (ed->nlines <= 0)
        return;
    if (ed->cursor_line < 0)
        ed->cursor_line = 0;
    if (ed->cursor_line >= ed->nlines)
        ed->cursor_line = ed->nlines - 1;
    if (ed->cursor_col < 0)
        ed->cursor_col = 0;
    if (ed->cursor_col > ed->lines[ed->cursor_line].len)
        ed->cursor_col = ed->lines[ed->cursor_line].len;
}

/* ----------------------------------------------------------- geometry */

static int gutter_width(const gui_editor *ed)
{
    int n, digits = 1;

    if (!ed->show_lineno)
        return 0;
    n = ed->nlines;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    if (digits < 3)
        digits = 3;
    return digits * GUI_FONT_W + 14;
}

static void compute_layout(const gui_editor *ed, gui_rc r, int *line_h_out,
                           int *gutter_w_out, int *vis_lines_out,
                           int *vis_cols_out, gui_rc *text_rc_out,
                           gui_rc *sb_rc_out)
{
    int lh = ed->line_h > 0 ? ed->line_h : GUI_FONT_H + 4;
    int gw = gutter_width(ed);
    int inner_h = r.h - 4;
    int vis_l = inner_h > lh ? inner_h / lh : 1;
    int has_sb = ed->nlines > vis_l;
    int text_x = r.x + 2 + gw + 4;
    int text_w = r.w - 4 - gw - 6 - (has_sb ? ED_BAR_W : 0);
    int vis_c;

    if (text_w < GUI_FONT_W)
        text_w = GUI_FONT_W;
    vis_c = text_w / GUI_FONT_W;
    if (vis_c < 1)
        vis_c = 1;
    if (line_h_out)
        *line_h_out = lh;
    if (gutter_w_out)
        *gutter_w_out = gw;
    if (vis_lines_out)
        *vis_lines_out = vis_l;
    if (vis_cols_out)
        *vis_cols_out = vis_c;
    if (text_rc_out)
        *text_rc_out = gui_mkrc(text_x, r.y + 2, text_w, r.h - 4);
    if (sb_rc_out)
        *sb_rc_out = has_sb
                         ? gui_mkrc(r.x + r.w - ED_BAR_W - 2, r.y + 2,
                                    ED_BAR_W, r.h - 4)
                         : gui_mkrc(0, 0, 0, 0);
}

static void clamp_scroll(gui_editor *ed, int vis_lines, int vis_cols)
{
    int max_y = ed->nlines - vis_lines;

    if (max_y < 0)
        max_y = 0;
    if (ed->scroll_y < 0)
        ed->scroll_y = 0;
    if (ed->scroll_y > max_y)
        ed->scroll_y = max_y;
    if (ed->scroll_x < 0)
        ed->scroll_x = 0;
    (void)vis_cols;
}

static void ensure_cursor_visible(gui_editor *ed, int vis_lines, int vis_cols)
{
    clamp_cursor(ed);
    if (ed->cursor_line < ed->scroll_y)
        ed->scroll_y = ed->cursor_line;
    if (ed->cursor_line >= ed->scroll_y + vis_lines)
        ed->scroll_y = ed->cursor_line - vis_lines + 1;
    if (ed->cursor_col < ed->scroll_x)
        ed->scroll_x = ed->cursor_col;
    if (ed->cursor_col >= ed->scroll_x + vis_cols)
        ed->scroll_x = ed->cursor_col - vis_cols + 1;
    clamp_scroll(ed, vis_lines, vis_cols);
}

/* ----------------------------------------------------------- selection */

static void norm_sel(const gui_editor *ed, int *sl, int *sc, int *el, int *ec)
{
    int a_l = ed->sel_line, a_c = ed->sel_col;
    int b_l = ed->cursor_line, b_c = ed->cursor_col;

    if (a_l > b_l || (a_l == b_l && a_c > b_c)) {
        *sl = b_l;
        *sc = b_c;
        *el = a_l;
        *ec = a_c;
    } else {
        *sl = a_l;
        *sc = a_c;
        *el = b_l;
        *ec = b_c;
    }
}

static void delete_selection(gui_editor *ed)
{
    int sl, sc, el, ec, i;
    gui_editor_line *first, *last;

    if (!ed->sel_active)
        return;
    norm_sel(ed, &sl, &sc, &el, &ec);
    if (sl < 0)
        sl = 0;
    if (el >= ed->nlines)
        el = ed->nlines - 1;
    first = &ed->lines[sl];
    last = &ed->lines[el];
    if (sc > first->len)
        sc = first->len;
    if (ec > last->len)
        ec = last->len;

    if (sl == el) {
        if (ec > sc) {
            memmove(first->text + sc, first->text + ec,
                    (unsigned long)(first->len - ec + 1));
            first->len -= (ec - sc);
        }
    } else {
        int tail = last->len - ec;

        if (line_reserve(first, sc + tail + 1) == 0) {
            if (tail > 0)
                memcpy(first->text + sc, last->text + ec,
                       (unsigned long)tail);
            first->len = sc + tail;
            first->text[first->len] = '\0';
        }
        for (i = el; i > sl; i--)
            remove_line(ed, i);
    }
    ed->cursor_line = sl;
    ed->cursor_col = sc;
    ed->sel_active = 0;
    ed->modified = 1;
}

static void copy_selection_to_talon(const gui_editor *ed)
{
    int sl, sc, el, ec, i, cap = 0, pos = 0;
    char *buf;

    if (!ed->sel_active) {
        const gui_editor_line *ln = &ed->lines[ed->cursor_line];
        if (ln->len > 0)
            (void)gui_clipboard_copy(ln->text, ln->len, "Editor");
        return;
    }
    norm_sel(ed, &sl, &sc, &el, &ec);
    for (i = sl; i <= el; i++)
        cap += ed->lines[i].len + 2;
    if (cap <= 0)
        return;
    if (cap > K_PARCEL_MAX_BYTES)
        cap = K_PARCEL_MAX_BYTES;
    buf = (char *)malloc((unsigned long)cap + 1);
    if (!buf)
        return;

    for (i = sl; i <= el && pos < cap; i++) {
        const gui_editor_line *ln = &ed->lines[i];
        int a = (i == sl) ? sc : 0;
        int b = (i == el) ? ec : ln->len;
        int take;

        if (a < 0)
            a = 0;
        if (b > ln->len)
            b = ln->len;
        take = b - a;
        if (take > 0) {
            if (pos + take > cap)
                take = cap - pos;
            memcpy(buf + pos, ln->text + a, (unsigned long)take);
            pos += take;
        }
        if (i < el && pos < cap)
            buf[pos++] = '\n';
    }
    if (pos > 0)
        (void)gui_clipboard_copy(buf, pos, "Editor");
    free(buf);
}

/* ------------------------------------------------------------ editing */

static void insert_char_raw(gui_editor *ed, char c)
{
    gui_editor_line *ln;

    if (ed->readonly)
        return;
    clamp_cursor(ed);
    ln = &ed->lines[ed->cursor_line];
    if (line_reserve(ln, ln->len + 2) != 0)
        return;
    memmove(ln->text + ed->cursor_col + 1, ln->text + ed->cursor_col,
            (unsigned long)(ln->len - ed->cursor_col + 1));
    ln->text[ed->cursor_col] = c;
    ln->len++;
    ed->cursor_col++;
    ed->modified = 1;
}

static void split_at_cursor(gui_editor *ed, int auto_indent)
{
    gui_editor_line *ln;
    int indent = 0, tail, i;

    if (ed->readonly)
        return;
    clamp_cursor(ed);
    ln = &ed->lines[ed->cursor_line];
    if (auto_indent) {
        while (indent < ed->cursor_col && ln->text[indent] == ' ')
            indent++;
    }
    tail = ln->len - ed->cursor_col;
    if (insert_new_line(ed, ed->cursor_line + 1, 0, 0) != 0)
        return;
    /* Re-fetch ln because lines array may have reallocated. */
    ln = &ed->lines[ed->cursor_line];
    if (line_reserve(&ed->lines[ed->cursor_line + 1], indent + tail + 1) == 0) {
        gui_editor_line *next = &ed->lines[ed->cursor_line + 1];

        for (i = 0; i < indent; i++)
            next->text[i] = ' ';
        if (tail > 0)
            memcpy(next->text + indent, ln->text + ed->cursor_col,
                   (unsigned long)tail);
        next->len = indent + tail;
        next->text[next->len] = '\0';
    }
    ln->len = ed->cursor_col;
    ln->text[ln->len] = '\0';
    ed->cursor_line++;
    ed->cursor_col = indent;
    ed->modified = 1;
}

static void insert_text_block(gui_editor *ed, const char *text, int len)
{
    int i;

    if (ed->readonly || !text || len <= 0)
        return;
    if (ed->sel_active)
        delete_selection(ed);
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (ch == '\r')
            continue;
        if (ch == '\n') {
            split_at_cursor(ed, 0);
        } else if (ch == '\t') {
            int sp = ed->tab_size - (ed->cursor_col % ed->tab_size);
            int k;
            for (k = 0; k < sp; k++)
                insert_char_raw(ed, ' ');
        } else if (ch >= 32 && ch < 127) {
            insert_char_raw(ed, (char)ch);
        }
    }
}

static void backspace_at_cursor(gui_editor *ed)
{
    gui_editor_line *ln, *prev;
    int prev_len;

    if (ed->readonly)
        return;
    if (ed->sel_active) {
        delete_selection(ed);
        return;
    }
    clamp_cursor(ed);
    if (ed->cursor_col > 0) {
        ln = &ed->lines[ed->cursor_line];
        memmove(ln->text + ed->cursor_col - 1, ln->text + ed->cursor_col,
                (unsigned long)(ln->len - ed->cursor_col + 1));
        ln->len--;
        ed->cursor_col--;
        ed->modified = 1;
    } else if (ed->cursor_line > 0) {
        prev = &ed->lines[ed->cursor_line - 1];
        ln = &ed->lines[ed->cursor_line];
        prev_len = prev->len;
        if (line_reserve(prev, prev->len + ln->len + 1) == 0) {
            if (ln->len > 0)
                memcpy(prev->text + prev->len, ln->text,
                       (unsigned long)ln->len);
            prev->len += ln->len;
            prev->text[prev->len] = '\0';
            remove_line(ed, ed->cursor_line);
            ed->cursor_line--;
            ed->cursor_col = prev_len;
            ed->modified = 1;
        }
    }
}

static void delete_at_cursor(gui_editor *ed)
{
    gui_editor_line *ln, *next;

    if (ed->readonly)
        return;
    if (ed->sel_active) {
        delete_selection(ed);
        return;
    }
    clamp_cursor(ed);
    ln = &ed->lines[ed->cursor_line];
    if (ed->cursor_col < ln->len) {
        memmove(ln->text + ed->cursor_col, ln->text + ed->cursor_col + 1,
                (unsigned long)(ln->len - ed->cursor_col));
        ln->len--;
        ed->modified = 1;
    } else if (ed->cursor_line + 1 < ed->nlines) {
        next = &ed->lines[ed->cursor_line + 1];
        if (line_reserve(ln, ln->len + next->len + 1) == 0) {
            if (next->len > 0)
                memcpy(ln->text + ln->len, next->text,
                       (unsigned long)next->len);
            ln->len += next->len;
            ln->text[ln->len] = '\0';
            remove_line(ed, ed->cursor_line + 1);
            ed->modified = 1;
        }
    }
}

/* ------------------------------------------------------------- public API */

void gui_editor_init(gui_editor *ed)
{
    if (!ed)
        return;
    memset(ed, 0, sizeof(*ed));
    ed->show_lineno = 1;
    ed->focus = 1;
    ed->tab_size = 4;
    ed->line_h = GUI_FONT_H + 4;
    if (lines_reserve(ed, 1) == 0) {
        if (line_init(&ed->lines[0], "", 0) == 0)
            ed->nlines = 1;
    }
}

gui_editor *gui_editor_create(void)
{
    gui_editor *ed = (gui_editor *)malloc(sizeof(*ed));

    if (!ed)
        return 0;
    gui_editor_init(ed);
    ed->heap_owned = 1;
    return ed;
}

void gui_editor_destroy(gui_editor *ed)
{
    int owned;

    if (!ed)
        return;
    owned = ed->heap_owned;
    free_all_lines(ed);
    if (owned)
        free(ed);
}

int gui_editor_set_text(gui_editor *ed, const char *text)
{
    const char *p;

    if (!ed)
        return -1;
    free_all_lines(ed);
    if (lines_reserve(ed, 1) != 0)
        return -1;

    if (!text || !*text) {
        if (line_init(&ed->lines[0], "", 0) != 0)
            return -1;
        ed->nlines = 1;
    } else {
        p = text;
        for (;;) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);

            while (len > 0 && p[len - 1] == '\r')
                len--;
            if (insert_new_line(ed, ed->nlines, p, len) != 0)
                break;
            if (!nl)
                break;
            p = nl + 1;
        }
        if (ed->nlines == 0 && line_init(&ed->lines[0], "", 0) == 0)
            ed->nlines = 1;
    }

    ed->cursor_line = 0;
    ed->cursor_col = 0;
    ed->sel_active = 0;
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->modified = 0;
    return 0;
}

int gui_editor_get_text(const gui_editor *ed, char *buf, int cap)
{
    int i, total = 0, pos = 0;

    if (!ed || ed->nlines <= 0) {
        if (buf && cap > 0)
            buf[0] = '\0';
        return 0;
    }
    for (i = 0; i < ed->nlines; i++) {
        const gui_editor_line *ln = &ed->lines[i];

        if (buf && cap > 1 && pos < cap - 1) {
            int take = ln->len;
            if (pos + take > cap - 1)
                take = cap - 1 - pos;
            if (take > 0)
                memcpy(buf + pos, ln->text, (unsigned long)take);
            pos += take;
            if (i + 1 < ed->nlines && pos < cap - 1)
                buf[pos++] = '\n';
        }
        total += ln->len + (i + 1 < ed->nlines ? 1 : 0);
    }
    if (buf && cap > 0)
        buf[pos] = '\0';
    return total;
}

void gui_editor_set_syntax(gui_editor *ed, int syntax)
{
    if (!ed)
        return;
    if (syntax < GUI_SYNTAX_NONE || syntax > GUI_SYNTAX_KREC)
        syntax = GUI_SYNTAX_NONE;
    ed->syntax = syntax;
}

int gui_editor_syntax_for_path(const char *path)
{
    const char *dot;

    if (!path)
        return GUI_SYNTAX_NONE;
    dot = strrchr(path, '.');
    if (!dot)
        return GUI_SYNTAX_NONE;
    dot++;
    if (streq_nocase(dot, "c") || streq_nocase(dot, "h") ||
        streq_nocase(dot, "cc") || streq_nocase(dot, "cpp"))
        return GUI_SYNTAX_C;
    if (streq_nocase(dot, "js") || streq_nocase(dot, "mjs") ||
        streq_nocase(dot, "json"))
        return GUI_SYNTAX_JS;
    if (streq_nocase(dot, "html") || streq_nocase(dot, "htm") ||
        streq_nocase(dot, "xml"))
        return GUI_SYNTAX_HTML;
    if (streq_nocase(dot, "css"))
        return GUI_SYNTAX_CSS;
    if (streq_nocase(dot, "sh") || streq_nocase(dot, "bash"))
        return GUI_SYNTAX_SH;
    if (streq_nocase(dot, "krec") || streq_nocase(dot, "conf") ||
        streq_nocase(dot, "ini") || streq_nocase(dot, "toml"))
        return GUI_SYNTAX_KREC;
    return GUI_SYNTAX_NONE;
}

/* ----------------------------------------------------------- events */

static void point_to_pos(const gui_editor *ed, gui_rc text_rc, int lh,
                         int px, int py, int *out_line, int *out_col)
{
    int line = ed->scroll_y + (py - text_rc.y) / lh;
    int col;

    if (line < 0)
        line = 0;
    if (line >= ed->nlines)
        line = ed->nlines - 1;
    col = ed->scroll_x + (px - text_rc.x + GUI_FONT_W / 2) / GUI_FONT_W;
    if (col < 0)
        col = 0;
    if (col > ed->lines[line].len)
        col = ed->lines[line].len;
    *out_line = line;
    *out_col = col;
}

int gui_editor_event(gui_editor *ed, gui_rc r, gui_ui *ui)
{
    int lh, gw, vis_lines, vis_cols, changed = 0;
    gui_rc text_rc, sb_rc;

    if (!ed || !ui || ed->nlines <= 0 || r.w <= 0 || r.h <= 0)
        return 0;
    clamp_cursor(ed);
    compute_layout(ed, r, &lh, &gw, &vis_lines, &vis_cols, &text_rc, &sb_rc);
    clamp_scroll(ed, vis_lines, vis_cols);

    if (ui->wheel && gui_hit(r, ui->mx, ui->my)) {
        ed->scroll_y -= ui->wheel * 3;
        clamp_scroll(ed, vis_lines, vis_cols);
        ui->wheel = 0;
        changed |= 1;
    }

    if (ui->down) {
        if (gui_hit(r, ui->down_x, ui->down_y)) {
            ed->focus = 1;
            if (gui_hit(text_rc, ui->down_x, ui->down_y)) {
                int l, c;

                point_to_pos(ed, text_rc, lh, ui->down_x, ui->down_y, &l, &c);
                ed->cursor_line = l;
                ed->cursor_col = c;
                ed->sel_line = l;
                ed->sel_col = c;
                ed->sel_active = 0;
            }
            changed |= 1;
        } else {
            if (ed->focus)
                changed |= 1;
            ed->focus = 0;
        }
    }

    if (ui->dbl && gui_hit(text_rc, ui->up_x, ui->up_y)) {
        int l, c, a, b;
        const gui_editor_line *ln;

        point_to_pos(ed, text_rc, lh, ui->up_x, ui->up_y, &l, &c);
        ln = &ed->lines[l];
        a = c;
        b = c;
        while (a > 0 && ch_alnum((unsigned char)ln->text[a - 1]))
            a--;
        while (b < ln->len && ch_alnum((unsigned char)ln->text[b]))
            b++;
        if (b > a) {
            ed->sel_line = l;
            ed->sel_col = a;
            ed->cursor_line = l;
            ed->cursor_col = b;
            ed->sel_active = 1;
            changed |= 1;
        }
    } else if ((ui->buttons & (unsigned)K_MOUSE_LEFT) &&
               gui_hit(text_rc, ui->down_x, ui->down_y) &&
               (ui->mx != ui->down_x || ui->my != ui->down_y)) {
        int l, c;

        point_to_pos(ed, text_rc, lh, ui->mx, ui->my, &l, &c);
        if (l != ed->cursor_line || c != ed->cursor_col) {
            ed->cursor_line = l;
            ed->cursor_col = c;
            ed->sel_active = (ed->cursor_line != ed->sel_line ||
                              ed->cursor_col != ed->sel_col);
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            changed |= 1;
        }
    }

    if (ed->focus && ui->key) {
        int k = ui->key;
        int handled = 1;
        int text_edited = 0;

        switch (k) {
        case KEY_UP:
            ed->sel_active = 0;
            if (ed->cursor_line > 0)
                ed->cursor_line--;
            clamp_cursor(ed);
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_DOWN:
            ed->sel_active = 0;
            if (ed->cursor_line + 1 < ed->nlines)
                ed->cursor_line++;
            clamp_cursor(ed);
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_LEFT:
            ed->sel_active = 0;
            if (ed->cursor_col > 0) {
                ed->cursor_col--;
            } else if (ed->cursor_line > 0) {
                ed->cursor_line--;
                ed->cursor_col = ed->lines[ed->cursor_line].len;
            }
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_RIGHT:
            ed->sel_active = 0;
            if (ed->cursor_col < ed->lines[ed->cursor_line].len) {
                ed->cursor_col++;
            } else if (ed->cursor_line + 1 < ed->nlines) {
                ed->cursor_line++;
                ed->cursor_col = 0;
            }
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_HOME:
            ed->sel_active = 0;
            ed->cursor_col = 0;
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_END:
        case 5: /* ctrl-E */
            ed->sel_active = 0;
            ed->cursor_col = ed->lines[ed->cursor_line].len;
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_PGUP:
            ed->sel_active = 0;
            ed->cursor_line -= vis_lines;
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_PGDN:
            ed->sel_active = 0;
            ed->cursor_line += vis_lines;
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case 1: /* ctrl-A: select all */
            ed->sel_line = 0;
            ed->sel_col = 0;
            ed->cursor_line = ed->nlines - 1;
            ed->cursor_col = ed->lines[ed->cursor_line].len;
            ed->sel_active = (ed->cursor_line > 0 || ed->cursor_col > 0);
            break;
        case 3: /* ctrl-C: copy to Talon */
            copy_selection_to_talon(ed);
            break;
        case 24: /* ctrl-X: cut to Talon */
            copy_selection_to_talon(ed);
            if (!ed->readonly) {
                if (ed->sel_active) {
                    delete_selection(ed);
                } else if (ed->nlines > 1) {
                    remove_line(ed, ed->cursor_line);
                    clamp_cursor(ed);
                    ed->modified = 1;
                } else {
                    ed->lines[0].len = 0;
                    ed->lines[0].text[0] = '\0';
                    ed->cursor_col = 0;
                    ed->modified = 1;
                }
                text_edited = 1;
                ensure_cursor_visible(ed, vis_lines, vis_cols);
            }
            break;
        case 22: /* ctrl-V: paste from Talon */
            if (!ed->readonly) {
                char *pbuf = (char *)malloc(4096);
                if (pbuf) {
                    int plen = gui_clipboard_paste(pbuf, 4096);
                    if (plen > 0) {
                        insert_text_block(ed, pbuf, plen);
                        text_edited = 1;
                    }
                    free(pbuf);
                    ensure_cursor_visible(ed, vis_lines, vis_cols);
                }
            }
            break;
        case 11: /* ctrl-K: delete current line */
            if (!ed->readonly) {
                ed->sel_active = 0;
                if (ed->nlines > 1) {
                    remove_line(ed, ed->cursor_line);
                    clamp_cursor(ed);
                } else {
                    ed->lines[0].len = 0;
                    ed->lines[0].text[0] = '\0';
                    ed->cursor_col = 0;
                }
                ed->modified = 1;
                text_edited = 1;
                ensure_cursor_visible(ed, vis_lines, vis_cols);
            }
            break;
        case 8:
        case 127:
            if (!ed->readonly) {
                backspace_at_cursor(ed);
                text_edited = 1;
            }
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case KEY_DELETE:
            if (!ed->readonly) {
                delete_at_cursor(ed);
                text_edited = 1;
            }
            ensure_cursor_visible(ed, vis_lines, vis_cols);
            break;
        case '\n':
        case '\r':
            if (!ed->readonly) {
                if (ed->sel_active)
                    delete_selection(ed);
                split_at_cursor(ed, 1);
                text_edited = 1;
                ensure_cursor_visible(ed, vis_lines, vis_cols);
            }
            break;
        case '\t':
            if (!ed->readonly) {
                int sp = ed->tab_size - (ed->cursor_col % ed->tab_size);
                int s;
                if (ed->sel_active)
                    delete_selection(ed);
                for (s = 0; s < sp; s++)
                    insert_char_raw(ed, ' ');
                text_edited = 1;
                ensure_cursor_visible(ed, vis_lines, vis_cols);
            }
            break;
        default:
            if (k >= 32 && k < 127 && !ed->readonly) {
                if (ed->sel_active)
                    delete_selection(ed);
                insert_char_raw(ed, (char)k);
                text_edited = 1;
                ensure_cursor_visible(ed, vis_lines, vis_cols);
            } else {
                handled = 0;
            }
            break;
        }
        if (handled) {
            ui->key = 0;
            changed |= 1;
            if (text_edited)
                changed |= 2;
        }
    }
    return changed;
}

/* ----------------------------------------------------- syntax coloring */

struct syn_palette {
    uint32_t text;
    uint32_t keyword;
    uint32_t type_tok;
    uint32_t string;
    uint32_t number;
    uint32_t comment;
    uint32_t prep;
    uint32_t punct;
};

static void build_syn_palette(struct syn_palette *p)
{
    uint32_t surf = GUI_SURFACE;
    int light = (GUI_RED(surf) + GUI_GREEN(surf) + GUI_BLUE(surf)) > 384;

    p->text = GUI_TEXT;
    p->keyword = light ? 0x7C3AEDU : 0xC084FCU;
    p->type_tok = GUI_ACCENT;
    p->string = light ? 0x15803DU : 0x4ADE80U;
    p->number = light ? 0xB45309U : 0xFBBF24U;
    p->comment = GUI_TEXT_DIM;
    p->prep = light ? 0x0D9488U : 0x2DD4BFU;
    p->punct = light ? 0x475569U : 0x94A3B8U;
}

static int word_in_list(const char *w, int len, const char *const *list)
{
    int i;

    for (i = 0; list[i]; i++) {
        if ((int)strlen(list[i]) == len &&
            strncmp(w, list[i], (unsigned long)len) == 0)
            return 1;
    }
    return 0;
}

static int is_c_keyword(const char *w, int len)
{
    static const char *const kw[] = {
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "sizeof", "struct", "union",
        "enum", "typedef", "static", "extern", "const", "volatile", "inline",
        "register", "NULL", "true", "false", 0
    };
    return word_in_list(w, len, kw);
}

static int is_c_type(const char *w, int len)
{
    static const char *const ty[] = {
        "void", "char", "short", "int", "long", "unsigned", "signed",
        "float", "double", "bool", "size_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", 0
    };
    return word_in_list(w, len, ty);
}

static int is_js_keyword(const char *w, int len)
{
    static const char *const kw[] = {
        "function", "const", "let", "var", "return", "if", "else", "for",
        "while", "do", "switch", "case", "default", "break", "continue",
        "new", "this", "typeof", "instanceof", "in", "of", "true", "false",
        "null", "undefined", "try", "catch", "finally", "throw", "async",
        "await", "class", "import", "export", 0
    };
    return word_in_list(w, len, kw);
}

static int is_sh_keyword(const char *w, int len)
{
    static const char *const kw[] = {
        "if", "then", "else", "elif", "fi", "for", "in", "do", "done",
        "while", "until", "case", "esac", "function", "return", "exit",
        "export", "local", "cd", "echo", "set", "unset", "true", "false", 0
    };
    return word_in_list(w, len, kw);
}

/* Advance multi-line block comment state across one line without drawing. */
static int scan_comment_line(const char *s, int len, int syntax, int in_comment)
{
    int i = 0;

    if (syntax == GUI_SYNTAX_HTML) {
        while (i < len) {
            if (in_comment) {
                if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' &&
                    s[i + 2] == '>') {
                    in_comment = 0;
                    i += 3;
                } else {
                    i++;
                }
            } else if (i + 3 < len && s[i] == '<' && s[i + 1] == '!' &&
                       s[i + 2] == '-' && s[i + 3] == '-') {
                in_comment = 1;
                i += 4;
            } else {
                i++;
            }
        }
        return in_comment;
    }

    if (syntax != GUI_SYNTAX_C && syntax != GUI_SYNTAX_JS &&
        syntax != GUI_SYNTAX_CSS)
        return 0;

    while (i < len) {
        if (in_comment) {
            if (i + 1 < len && s[i] == '*' && s[i + 1] == '/') {
                in_comment = 0;
                i += 2;
            } else {
                i++;
            }
        } else if (s[i] == '"' || s[i] == '\'' ||
                   (syntax == GUI_SYNTAX_JS && s[i] == '`')) {
            char q = s[i++];
            while (i < len && s[i] != q) {
                if (s[i] == '\\' && i + 1 < len)
                    i++;
                i++;
            }
            if (i < len)
                i++;
        } else if (i + 1 < len && s[i] == '/' && s[i + 1] == '/') {
            if (syntax != GUI_SYNTAX_CSS)
                break;
            i++;
        } else if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            in_comment = 1;
            i += 2;
        } else {
            i++;
        }
    }
    return in_comment;
}

static void draw_tok_span(gui_window *win, const gui_editor *ed, int text_x,
                          int y, int vis_cols, const char *s, int start,
                          int count, uint32_t color)
{
    int k;

    for (k = 0; k < count; k++) {
        int col = start + k;

        if (col < ed->scroll_x)
            continue;
        if (col >= ed->scroll_x + vis_cols)
            break;
        gui_char(win, text_x + (col - ed->scroll_x) * GUI_FONT_W, y,
                 s[col], color, GUI_TRANSPARENT);
    }
}

static void draw_line_syntax(gui_window *win, const gui_editor *ed,
                             const gui_editor_line *ln, int text_x, int y,
                             int vis_cols, int *in_comment,
                             const struct syn_palette *pal)
{
    const char *s = ln->text;
    int len = ln->len;
    int syn = ed->syntax;
    int i = 0;
    int in_tag = 0;

    if (syn == GUI_SYNTAX_NONE) {
        draw_tok_span(win, ed, text_x, y, vis_cols, s, 0, len, pal->text);
        return;
    }

    if (syn == GUI_SYNTAX_C) {
        int lead = 0;
        while (lead < len && (s[lead] == ' ' || s[lead] == '\t'))
            lead++;
        if (!*in_comment && lead < len && s[lead] == '#') {
            draw_tok_span(win, ed, text_x, y, vis_cols, s, 0, len, pal->prep);
            return;
        }
    }

    while (i < len) {
        if (syn == GUI_SYNTAX_HTML) {
            if (*in_comment) {
                int start = i;
                while (i < len) {
                    if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' &&
                        s[i + 2] == '>') {
                        i += 3;
                        *in_comment = 0;
                        break;
                    }
                    i++;
                }
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->comment);
                continue;
            }
            if (i + 3 < len && s[i] == '<' && s[i + 1] == '!' &&
                s[i + 2] == '-' && s[i + 3] == '-') {
                int start = i;
                i += 4;
                *in_comment = 1;
                while (i < len) {
                    if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' &&
                        s[i + 2] == '>') {
                        i += 3;
                        *in_comment = 0;
                        break;
                    }
                    i++;
                }
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->comment);
                continue;
            }
            if (s[i] == '<') {
                int start = i++;
                if (i < len && (s[i] == '/' || s[i] == '!'))
                    i++;
                while (i < len && (ch_alnum((unsigned char)s[i]) ||
                                   s[i] == '-' || s[i] == '_'))
                    i++;
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->type_tok);
                in_tag = 1;
                continue;
            }
            if (in_tag && s[i] == '>') {
                draw_tok_span(win, ed, text_x, y, vis_cols, s, i, 1,
                              pal->type_tok);
                i++;
                in_tag = 0;
                continue;
            }
            if (in_tag && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i];
                int start = i++;
                while (i < len && s[i] != q)
                    i++;
                if (i < len)
                    i++;
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->string);
                continue;
            }
            if (in_tag && ch_alpha((unsigned char)s[i])) {
                int start = i;
                while (i < len && (ch_alnum((unsigned char)s[i]) ||
                                   s[i] == '-'))
                    i++;
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->prep);
                continue;
            }
            if (s[i] == '&') {
                int start = i++;
                while (i < len && i - start < 10 &&
                       ch_alnum((unsigned char)s[i]))
                    i++;
                if (i < len && s[i] == ';')
                    i++;
                draw_tok_span(win, ed, text_x, y, vis_cols, s, start,
                              i - start, pal->number);
                continue;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, i, 1,
                          in_tag ? pal->punct : pal->text);
            i++;
            continue;
        }

        /* Block comment continuation (C, JS, CSS) */
        if (*in_comment) {
            int start = i;
            while (i < len) {
                if (i + 1 < len && s[i] == '*' && s[i + 1] == '/') {
                    i += 2;
                    *in_comment = 0;
                    break;
                }
                i++;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->comment);
            continue;
        }

        /* Single-line comments */
        if (((syn == GUI_SYNTAX_C || syn == GUI_SYNTAX_JS ||
              syn == GUI_SYNTAX_KREC) &&
             i + 1 < len && s[i] == '/' && s[i + 1] == '/') ||
            ((syn == GUI_SYNTAX_SH || syn == GUI_SYNTAX_KREC) &&
             s[i] == '#')) {
            draw_tok_span(win, ed, text_x, y, vis_cols, s, i, len - i,
                          pal->comment);
            break;
        }

        /* Block comment start */
        if ((syn == GUI_SYNTAX_C || syn == GUI_SYNTAX_JS ||
             syn == GUI_SYNTAX_CSS) &&
            i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            int start = i;
            i += 2;
            *in_comment = 1;
            while (i < len) {
                if (i + 1 < len && s[i] == '*' && s[i + 1] == '/') {
                    i += 2;
                    *in_comment = 0;
                    break;
                }
                i++;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->comment);
            continue;
        }

        /* Strings */
        if (s[i] == '"' || s[i] == '\'' ||
            (syn == GUI_SYNTAX_JS && s[i] == '`')) {
            char q = s[i];
            int start = i++;
            while (i < len && s[i] != q) {
                if (s[i] == '\\' && i + 1 < len)
                    i++;
                i++;
            }
            if (i < len)
                i++;
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->string);
            continue;
        }

        /* Shell $VAR */
        if (syn == GUI_SYNTAX_SH && s[i] == '$') {
            int start = i++;
            if (i < len && s[i] == '{') {
                while (i < len && s[i] != '}')
                    i++;
                if (i < len)
                    i++;
            } else {
                while (i < len && ch_alnum((unsigned char)s[i]))
                    i++;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->prep);
            continue;
        }

        /* KREC [section] */
        if (syn == GUI_SYNTAX_KREC && s[i] == '[') {
            int start = i++;
            while (i < len && s[i] != ']')
                i++;
            if (i < len)
                i++;
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->type_tok);
            continue;
        }

        /* CSS #hex color */
        if (syn == GUI_SYNTAX_CSS && s[i] == '#') {
            int start = i++;
            while (i < len && ch_hex((unsigned char)s[i]))
                i++;
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->number);
            continue;
        }

        /* Numbers */
        if (ch_digit((unsigned char)s[i]) &&
            (i == 0 || !ch_alnum((unsigned char)s[i - 1]))) {
            int start = i;
            if (i + 1 < len && s[i] == '0' &&
                (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                while (i < len && ch_hex((unsigned char)s[i]))
                    i++;
            } else {
                while (i < len &&
                       (ch_digit((unsigned char)s[i]) || s[i] == '.'))
                    i++;
            }
            if (syn == GUI_SYNTAX_CSS) {
                while (i < len && (ch_alpha((unsigned char)s[i]) ||
                                   s[i] == '%'))
                    i++;
            } else {
                while (i < len && (s[i] == 'u' || s[i] == 'U' ||
                                   s[i] == 'l' || s[i] == 'L'))
                    i++;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          pal->number);
            continue;
        }

        /* Identifiers / keywords */
        if (ch_alpha((unsigned char)s[i])) {
            int start = i;
            uint32_t col = pal->text;

            while (i < len &&
                   (ch_alnum((unsigned char)s[i]) ||
                    (syn == GUI_SYNTAX_CSS && s[i] == '-')))
                i++;
            if (syn == GUI_SYNTAX_C) {
                if (is_c_keyword(s + start, i - start))
                    col = pal->keyword;
                else if (is_c_type(s + start, i - start))
                    col = pal->type_tok;
            } else if (syn == GUI_SYNTAX_JS) {
                if (is_js_keyword(s + start, i - start))
                    col = pal->keyword;
            } else if (syn == GUI_SYNTAX_SH) {
                if (is_sh_keyword(s + start, i - start))
                    col = pal->keyword;
            } else if (syn == GUI_SYNTAX_CSS) {
                int j = i;
                while (j < len && s[j] == ' ')
                    j++;
                if (j < len && s[j] == ':')
                    col = pal->type_tok;
            } else if (syn == GUI_SYNTAX_KREC) {
                int j = i;
                while (j < len && s[j] == ' ')
                    j++;
                if (j < len && (s[j] == '=' || s[j] == ':'))
                    col = pal->prep;
                else if (streq_nocase(s + start, "true") ||
                         streq_nocase(s + start, "false") ||
                         streq_nocase(s + start, "null"))
                    col = pal->keyword;
            }
            draw_tok_span(win, ed, text_x, y, vis_cols, s, start, i - start,
                          col);
            continue;
        }

        /* Punctuation */
        {
            uint32_t pcol = pal->text;
            if (strchr("{}()[];:,=+-*/<>!&|^~%.", s[i]))
                pcol = pal->punct;
            draw_tok_span(win, ed, text_x, y, vis_cols, s, i, 1, pcol);
            i++;
        }
    }
}

void gui_editor_draw(gui_window *win, gui_rc r, gui_editor *ed, gui_ui *ui)
{
    int lh, gw, vis_lines, vis_cols, i, in_comment = 0;
    int sl = 0, sc = 0, el = 0, ec = 0;
    gui_rc text_rc, sb_rc;
    struct syn_palette pal;
    uint32_t gutter_bg, cur_line_bg, sel_bg;
    int is_light;

    if (!win || !ed || ed->nlines <= 0 || r.w <= 4 || r.h <= 4)
        return;
    clamp_cursor(ed);
    compute_layout(ed, r, &lh, &gw, &vis_lines, &vis_cols, &text_rc, &sb_rc);
    clamp_scroll(ed, vis_lines, vis_cols);
    build_syn_palette(&pal);

    is_light = (GUI_RED(GUI_SURFACE) + GUI_GREEN(GUI_SURFACE) +
                GUI_BLUE(GUI_SURFACE)) > 384;
    gutter_bg = gui_mix(GUI_PANEL, GUI_SUNKEN, 128);
    cur_line_bg = gui_shade(GUI_SUNKEN, is_light ? -4 : 6);
    sel_bg = gui_mix(GUI_ACCENT_DARK, GUI_SURFACE, 80);

    gui_round_rect(win, r, 7, GUI_SUNKEN);
    gui_round_frame(win, r, 7, ed->focus ? GUI_ACCENT : GUI_EDGE);

    if (gw > 0) {
        gui_push_clip_xy(win, r.x + 1, r.y + 1, gw, r.h - 2);
        gui_rect(win, r.x + 1, r.y + 1, gw, r.h - 2, gutter_bg);
        gui_rect(win, r.x + 1 + gw - 1, r.y + 1, 1, r.h - 2, GUI_BORDER);
        gui_pop_clip(win);
    }

    if (sb_rc.w > 0 && ui)
        gui_scrollbar(win, sb_rc, ed->nlines, vis_lines, &ed->scroll_y, ui);

    /* Advance block comment state up to scroll_y. */
    if (ed->syntax != GUI_SYNTAX_NONE) {
        int lim = ed->scroll_y < ed->nlines ? ed->scroll_y : ed->nlines;
        for (i = 0; i < lim; i++)
            in_comment = scan_comment_line(ed->lines[i].text,
                                           ed->lines[i].len,
                                           ed->syntax, in_comment);
    }
    if (ed->sel_active)
        norm_sel(ed, &sl, &sc, &el, &ec);

    gui_push_clip_xy(win, r.x + 1, r.y + 2,
                     r.w - 2 - (sb_rc.w > 0 ? ED_BAR_W + 2 : 0), r.h - 4);
    for (i = 0; i < vis_lines; i++) {
        int li = ed->scroll_y + i;
        int ly = text_rc.y + i * lh;
        int ty = ly + (lh - GUI_FONT_H) / 2;

        if (li >= ed->nlines)
            break;

        if (li == ed->cursor_line && ed->focus)
            gui_rect(win, text_rc.x - 4, ly, text_rc.w + 4, lh, cur_line_bg);

        if (gw > 0) {
            char lnum[16];
            int nw;

            snprintf(lnum, sizeof(lnum), "%d", li + 1);
            nw = gui_text_w(lnum);
            gui_text(win, r.x + gw - nw - 6, ty, lnum,
                     li == ed->cursor_line ? GUI_ACCENT : GUI_TEXT_DIM,
                     GUI_TRANSPARENT);
        }

        if (ed->sel_active && li >= sl && li <= el) {
            int sa = (li == sl) ? sc : 0;
            int sb = (li == el) ? ec : ed->lines[li].len;
            int va = sa > ed->scroll_x ? sa - ed->scroll_x : 0;
            int vb = sb - ed->scroll_x;

            if (vb > vis_cols)
                vb = vis_cols;
            if (vb > va)
                gui_rect(win, text_rc.x + va * GUI_FONT_W, ly,
                         (vb - va) * GUI_FONT_W, lh, sel_bg);
        }

        gui_push_clip(win, text_rc);
        draw_line_syntax(win, ed, &ed->lines[li], text_rc.x, ty, vis_cols,
                         &in_comment, &pal);
        gui_pop_clip(win);

        if (ed->focus && !ed->readonly && li == ed->cursor_line &&
            ((uptime_ms() / CARET_BLINK_MS) & 1) == 0) {
            int cc = ed->cursor_col - ed->scroll_x;
            if (cc >= 0 && cc <= vis_cols) {
                int cx = text_rc.x + cc * GUI_FONT_W;
                gui_rect(win, cx, ly + 1, 2, lh - 2, GUI_ACCENT);
            }
        }
    }
    gui_pop_clip(win);
}
