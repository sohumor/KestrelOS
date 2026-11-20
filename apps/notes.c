/* notes.c - Avionics Notes: Markdown Scratchpad & Flight Log for KestrelOS.
 *
 * Features:
 *   - Left note sidebar backed by /home/notes (auto-creating /home and
 *     /home/notes; falling back cleanly to /tmp/notes if non-root lacks write
 *     permission; also accepts an explicit file path CLI argument).
 *   - [+ New], [Delete], [Save (Ctrl+S)], [Insert Talon], [Send to Talon],
 *     and [Edit / Split / Preview] view mode toggle (Ctrl+P).
 *   - Edit pane powered by libgui's gui_editor (line numbers, syntax
 *     highlighting, KEV_MOUSE_WHEEL scroll, Talon copy/cut/paste).
 *   - Preview pane renders rich Markdown directly on the Avionics canvas:
 *     # / ## / ### AA headings, - / * bullets & task checkboxes, fenced
 *     ``` code blocks ``` in monospace cards, `inline code` pills, > quote
 *     bars, --- dividers, **bold**, *italic*, and [link](url) styling.
 *   - Full KEV_RESIZE dynamic reflow.
 *
 * usage: notes [file]
 */

#include <kestrel.h>
#include <kestrel_talon.h>
#include <krec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define MAX_NOTES    128
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

enum view_mode {
    VIEW_EDIT = 0,
    VIEW_SPLIT,
    VIEW_PREVIEW
};

enum pending_action {
    PEND_NONE = 0,
    PEND_NEW,
    PEND_SELECT,
    PEND_DELETE
};

struct note_entry {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    int external;
};

/* ---- state ----------------------------------------------------------- */

static char g_cwd[MAX_PATH_LEN] = "/";
static char notes_dir[MAX_PATH_LEN] = "/home/notes";

static struct note_entry notes[MAX_NOTES];
static int nnotes;
static int cur_idx = -1;
static char cur_path[MAX_PATH_LEN];
static char cur_name[MAX_NAME_LEN];

static gui_list note_list;
static gui_editor *ed;
static enum view_mode view_mode = VIEW_SPLIT;
static int preview_scroll;

static enum pending_action pending_act = PEND_NONE;
static int pending_sel_idx = -1;
static char status_msg[128] = "Ready";
static int need_repaint;
static int dirty = 1;

/* ---- path & directory helpers ---------------------------------------- */

static void normalize_path(const char *src, char *out, size_t outsz)
{
    char clean[MAX_PATH_LEN];
    char seg[MAX_PATH_LEN];
    const char *p = src;
    size_t len = 1;

    clean[0] = '/';
    clean[1] = '\0';
    while (*p) {
        size_t n = 0;
        while (*p == '/')
            p++;
        while (*p && *p != '/' && n + 1 < sizeof(seg))
            seg[n++] = *p++;
        seg[n] = '\0';
        if (n == 0)
            break;
        if (strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            while (len > 1 && clean[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            clean[len] = '\0';
            continue;
        }
        if (len > 1 && len + 1 < sizeof(clean))
            clean[len++] = '/';
        for (size_t i = 0; seg[i] && len + 1 < sizeof(clean); i++)
            clean[len++] = seg[i];
        clean[len] = '\0';
    }
    snprintf(out, outsz, "%s", clean);
}

static void resolve_path(const char *arg, char *out, size_t outsz)
{
    char joined[MAX_PATH_LEN];

    if (!arg || !arg[0]) {
        snprintf(out, outsz, "%s", g_cwd);
        return;
    }
    if (arg[0] == '/')
        snprintf(joined, sizeof(joined), "%s", arg);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(joined, sizeof(joined), "/%s", arg);
    else
        snprintf(joined, sizeof(joined), "%s/%s", g_cwd, arg);
    normalize_path(joined, out, outsz);
}

static int dir_is_writable(const struct k_stat *st)
{
    uint32_t uid = getuid_();
    uint32_t gid = getgid_();

    if (uid == 0)
        return 1;
    if (st->uid == uid && (st->mode & K_IWUSR))
        return 1;
    if (st->gid == gid && (st->mode & K_IWGRP))
        return 1;
    if (st->mode & K_IWOTH)
        return 1;
    return 0;
}

static int ensure_writable_dir(const char *parent, const char *dir)
{
    struct k_stat st;

    if (stat_(parent, &st) != 0) {
        if (mkdir_(parent) != 0)
            return -1;
    }
    if (stat_(parent, &st) != 0 || !st.is_dir)
        return -1;

    if (stat_(dir, &st) != 0) {
        if (mkdir_(dir) != 0)
            return -1;
    }
    if (stat_(dir, &st) != 0 || !st.is_dir)
        return -1;
    if (!dir_is_writable(&st))
        return -1;
    return 0;
}

static void init_notes_dir(void)
{
    if (ensure_writable_dir("/home", "/home/notes") == 0) {
        snprintf(notes_dir, sizeof(notes_dir), "/home/notes");
        return;
    }
    if (ensure_writable_dir("/tmp", "/tmp/notes") == 0) {
        snprintf(notes_dir, sizeof(notes_dir), "/tmp/notes");
        return;
    }
    snprintf(notes_dir, sizeof(notes_dir), "/tmp");
}

static const char *base_name(const char *path)
{
    const char *sl = strrchr(path, '/');
    return sl ? sl + 1 : path;
}

/* ---- note storage ---------------------------------------------------- */

static void sort_notes(void)
{
    int i, j;

    for (i = 1; i < nnotes; i++) {
        struct note_entry key = notes[i];
        for (j = i - 1; j >= 0 && strcmp(key.name, notes[j].name) < 0; j--)
            notes[j + 1] = notes[j];
        notes[j + 1] = key;
    }
}

static void reload_note_list(const char *select_path)
{
    struct k_dirent de;
    int i = 0;

    nnotes = 0;
    while (nnotes < MAX_NOTES && readdir_at(notes_dir, i++, &de) == 0) {
        if (de.is_dir)
            continue;
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
        de.name[sizeof(de.name) - 1] = '\0';
        snprintf(notes[nnotes].name, sizeof(notes[0].name), "%s", de.name);
        snprintf(notes[nnotes].path, sizeof(notes[0].path), "%s/%s",
                 notes_dir, de.name);
        notes[nnotes].external = 0;
        nnotes++;
    }
    sort_notes();

    cur_idx = -1;
    if (select_path && select_path[0]) {
        for (i = 0; i < nnotes; i++) {
            if (strcmp(notes[i].path, select_path) == 0) {
                cur_idx = i;
                break;
            }
        }
        if (cur_idx < 0 && nnotes < MAX_NOTES) {
            snprintf(notes[nnotes].name, sizeof(notes[0].name), "%s",
                     base_name(select_path));
            snprintf(notes[nnotes].path, sizeof(notes[0].path), "%s",
                     select_path);
            notes[nnotes].external = 1;
            cur_idx = nnotes++;
        }
    }
    note_list.count = nnotes;
    note_list.sel = cur_idx;
}

static int write_all_fd(int fd, const char *buf, unsigned long len)
{
    unsigned long off = 0;

    while (off < len) {
        long w = write(fd, buf + off, len - off);
        if (w <= 0)
            return -1;
        off += (unsigned long)w;
    }
    return 0;
}

static void update_editor_syntax(const char *path, const char *text)
{
    int syn = gui_editor_syntax_for_path(path);

    if (syn == GUI_SYNTAX_NONE && text && strncmp(text, "@krec", 5) == 0 &&
        (text[5] == ' ' || text[5] == '\t'))
        syn = GUI_SYNTAX_KREC;
    gui_editor_set_syntax(ed, syn);
}

static void open_note_path(const char *path)
{
    struct k_stat st;
    int fd;
    char *buf = 0;
    long got = 0;

    snprintf(cur_path, sizeof(cur_path), "%s", path);
    snprintf(cur_name, sizeof(cur_name), "%s", base_name(path));
    preview_scroll = 0;

    if (stat_(path, &st) == 0) {
        if (st.is_dir) {
            gui_editor_set_text(ed, "");
            snprintf(status_msg, sizeof(status_msg),
                     "%s is a directory", path);
            return;
        }
        uint32_t cap = st.size < 65536U ? st.size : 65536U;
        buf = (char *)malloc(cap + 1);
        if (buf) {
            fd = open(path, O_RDONLY);
            if (fd >= 0) {
                while ((uint32_t)got < cap) {
                    long n = read(fd, buf + got, cap - (uint32_t)got);
                    if (n <= 0)
                        break;
                    got += n;
                }
                close(fd);
            }
            buf[got] = '\0';
        }
    }

    if (buf) {
        gui_editor_set_text(ed, buf);
        update_editor_syntax(path, buf);
        free(buf);
        snprintf(status_msg, sizeof(status_msg), "Loaded %s (%ld B)",
                 cur_name, got);
    } else {
        gui_editor_set_text(ed, "");
        update_editor_syntax(path, "");
        snprintf(status_msg, sizeof(status_msg), "New note: %s", cur_name);
    }
    ed->modified = 0;
    ed->focus = (view_mode != VIEW_PREVIEW);
    reload_note_list(cur_path);
}

static int save_current_note(void)
{
    int len, fd, rc;
    char *buf;

    if (!cur_path[0])
        snprintf(cur_path, sizeof(cur_path), "%s/flight-log.md", notes_dir);
    snprintf(cur_name, sizeof(cur_name), "%s", base_name(cur_path));

    len = gui_editor_get_text(ed, 0, 0);
    buf = (char *)malloc((unsigned long)(len + 1));
    if (!buf) {
        snprintf(status_msg, sizeof(status_msg), "Out of memory");
        return -1;
    }
    gui_editor_get_text(ed, buf, len + 1);

    fd = open(cur_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(buf);
        snprintf(status_msg, sizeof(status_msg), "Cannot save %s", cur_path);
        return -1;
    }
    rc = (len > 0) ? write_all_fd(fd, buf, (unsigned long)len) : 0;
    close(fd);
    update_editor_syntax(cur_path, buf);
    free(buf);

    if (rc < 0) {
        snprintf(status_msg, sizeof(status_msg), "Write failed: %s", cur_path);
        return -1;
    }
    ed->modified = 0;
    reload_note_list(cur_path);
    snprintf(status_msg, sizeof(status_msg), "Saved %s (%d bytes)",
             cur_name, len);
    return 0;
}

static void create_new_note(void)
{
    char cand[MAX_PATH_LEN];
    struct k_stat st;
    int num = 1;
    static const char *starter =
        "# Flight Log\n\n"
        "- Status: **nominal**\n"
        "- Press `Ctrl+S` to save\n";

    for (num = 1; num < 999; num++) {
        snprintf(cand, sizeof(cand), "%s/note-%d.md", notes_dir, num);
        if (stat_(cand, &st) != 0)
            break;
    }
    {
        int fd = open(cand, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            write_all_fd(fd, starter, strlen(starter));
            close(fd);
        }
    }
    open_note_path(cand);
    snprintf(status_msg, sizeof(status_msg), "Created %s", cur_name);
}

static void delete_current_note(void)
{
    char target[MAX_PATH_LEN];

    if (!cur_path[0])
        return;
    snprintf(target, sizeof(target), "%s", cur_path);
    if (unlink_(target) != 0) {
        snprintf(status_msg, sizeof(status_msg), "Cannot delete %s", target);
        return;
    }
    reload_note_list(0);
    if (nnotes > 0) {
        int next = cur_idx >= 0 ? cur_idx : 0;
        if (next >= nnotes)
            next = nnotes - 1;
        open_note_path(notes[next].path);
    } else {
        snprintf(cur_path, sizeof(cur_path), "%s/flight-log.md", notes_dir);
        snprintf(cur_name, sizeof(cur_name), "flight-log.md");
        gui_editor_set_text(ed, "");
        ed->modified = 0;
        reload_note_list(cur_path);
    }
    snprintf(status_msg, sizeof(status_msg), "Deleted %s", base_name(target));
}

/* ---- Talon integration ----------------------------------------------- */

static int cursor_byte_offset(const gui_editor *e)
{
    int off = 0, i;

    for (i = 0; i < e->cursor_line && i < e->nlines; i++)
        off += e->lines[i].len + 1;
    if (e->cursor_line >= 0 && e->cursor_line < e->nlines) {
        int c = e->cursor_col;
        if (c > e->lines[e->cursor_line].len)
            c = e->lines[e->cursor_line].len;
        off += c;
    }
    return off;
}

static void offset_to_line_col(const char *text, int target,
                               int *line_out, int *col_out)
{
    int l = 0, c = 0, i;

    for (i = 0; i < target && text[i]; i++) {
        if (text[i] == '\n') {
            l++;
            c = 0;
        } else if (text[i] != '\r') {
            c++;
        }
    }
    *line_out = l;
    *col_out = c;
}

static void insert_talon_into_editor(void)
{
    struct k_parcel_info info;
    char *pbuf;
    long plen;
    int old_len, off, new_len, nl = 0, nc = 0;
    char *old_buf, *new_buf;

    if (parcel_get(0, &info, 0, 0) < 0 || info.size == 0) {
        snprintf(status_msg, sizeof(status_msg), "Talon clipboard is empty");
        return;
    }
    if (info.type == K_PARCEL_IMAGE) {
        snprintf(status_msg, sizeof(status_msg),
                 "Cannot insert binary image parcel");
        return;
    }
    uint32_t want = info.size < K_PARCEL_MAX_BYTES
                        ? info.size : (uint32_t)(K_PARCEL_MAX_BYTES - 1);
    pbuf = (char *)malloc(want + 1);
    if (!pbuf)
        return;
    plen = parcel_get(info.id, 0, pbuf, want);
    if (plen <= 0) {
        free(pbuf);
        snprintf(status_msg, sizeof(status_msg), "Failed reading Talon parcel");
        return;
    }
    pbuf[plen] = '\0';

    old_len = gui_editor_get_text(ed, 0, 0);
    off = cursor_byte_offset(ed);
    if (off > old_len)
        off = old_len;
    old_buf = (char *)malloc((unsigned long)(old_len + 1));
    new_buf = (char *)malloc((unsigned long)(old_len + plen + 2));
    if (!old_buf || !new_buf) {
        free(pbuf);
        free(old_buf);
        free(new_buf);
        return;
    }
    gui_editor_get_text(ed, old_buf, old_len + 1);
    memcpy(new_buf, old_buf, (unsigned long)off);
    memcpy(new_buf + off, pbuf, (unsigned long)plen);
    memcpy(new_buf + off + plen, old_buf + off,
           (unsigned long)(old_len - off + 1));
    new_len = off + (int)plen;

    gui_editor_set_text(ed, new_buf);
    offset_to_line_col(new_buf, new_len, &nl, &nc);
    ed->cursor_line = nl;
    ed->cursor_col = nc;
    ed->modified = 1;
    ed->focus = (view_mode != VIEW_PREVIEW);

    free(pbuf);
    free(old_buf);
    free(new_buf);
    snprintf(status_msg, sizeof(status_msg),
             "Inserted %ld B from Talon (#%u)", plen, info.id);
}

static void send_note_to_talon(void)
{
    char *buf = 0;
    int len = 0;
    uint32_t ptype = K_PARCEL_TEXT;
    int pid;

    if (ed->sel_active) {
        int a_l = ed->sel_line, a_c = ed->sel_col;
        int b_l = ed->cursor_line, b_c = ed->cursor_col;
        int cap = 0, pos = 0;

        if (a_l > b_l || (a_l == b_l && a_c > b_c)) {
            int tl = a_l, tc = a_c;
            a_l = b_l; a_c = b_c;
            b_l = tl;  b_c = tc;
        }
        for (int i = a_l; i <= b_l && i < ed->nlines; i++)
            cap += ed->lines[i].len + 2;
        if (cap > K_PARCEL_MAX_BYTES)
            cap = K_PARCEL_MAX_BYTES;
        if (cap > 0) {
            buf = (char *)malloc((unsigned long)(cap + 1));
            if (buf) {
                for (int i = a_l; i <= b_l && i < ed->nlines && pos < cap; i++) {
                    int sa = (i == a_l) ? a_c : 0;
                    int sb = (i == b_l) ? b_c : ed->lines[i].len;
                    if (sa < 0) sa = 0;
                    if (sb > ed->lines[i].len) sb = ed->lines[i].len;
                    int take = sb - sa;
                    if (take > 0) {
                        if (pos + take > cap) take = cap - pos;
                        memcpy(buf + pos, ed->lines[i].text + sa,
                               (unsigned long)take);
                        pos += take;
                    }
                    if (i < b_l && pos < cap)
                        buf[pos++] = '\n';
                }
                buf[pos] = '\0';
                len = pos;
            }
        }
    } else {
        len = gui_editor_get_text(ed, 0, 0);
        if (len > K_PARCEL_MAX_BYTES)
            len = K_PARCEL_MAX_BYTES;
        if (len > 0) {
            buf = (char *)malloc((unsigned long)(len + 1));
            if (buf)
                gui_editor_get_text(ed, buf, len + 1);
        }
    }

    if (!buf || len <= 0) {
        free(buf);
        snprintf(status_msg, sizeof(status_msg), "Note is empty");
        return;
    }
    if (len >= 6 && strncmp(buf, "@krec", 5) == 0 &&
        (buf[5] == ' ' || buf[5] == '\t'))
        ptype = K_PARCEL_KREC;
    pid = parcel_put(ptype, talon_type_default_mime(ptype),
                     cur_name[0] ? cur_name : "Notes", buf, (uint32_t)len);
    free(buf);
    if (pid >= 0)
        snprintf(status_msg, sizeof(status_msg),
                 "Sent %d B to Talon parcel #%d", len, pid);
    else
        snprintf(status_msg, sizeof(status_msg), "Send to Talon failed");
}

/* ---- Markdown Preview Canvas Renderer -------------------------------- */

struct md_ctx {
    gui_window *win;
    gui_rc view;
    int draw;
    int scroll_y;
    int cur_y;
};

static void md_draw_span(struct md_ctx *mc, int x, int y, int size, int weight,
                         int style, uint32_t fg, const char *s, int n,
                         int is_code, int is_link)
{
    const struct font *f = font_get(size, weight, style);
    int w = n * f->w;

    if (!mc->draw || y + f->h < mc->view.y || y > mc->view.y + mc->view.h)
        return;
    if (is_code) {
        gui_rc pill = gui_mkrc(x - 3, y, w + 6, f->h);
        gui_fill_round_rect(mc->win, pill, 4, GUI_SURFACE_ALT);
        gui_stroke_round_rect(mc->win, pill, 4, 1, GUI_EDGE);
        gui_draw_text_aa_n(mc->win, x, y, s, n, size, weight, FONT_ROMAN,
                           GUI_ACCENT, GUI_TRANSPARENT);
        return;
    }
    gui_draw_text_aa_n(mc->win, x, y, s, n, size, weight, style,
                       is_link ? GUI_ACCENT : fg, GUI_TRANSPARENT);
    if (is_link && w > 0)
        gui_rect(mc->win, x, y + f->h - 2, w, 1, GUI_ACCENT);
}

/* Word-wrap and optionally draw inline Markdown spans:
 *   `code`, **bold**, *italic*, [label](url). */
static void md_inline(struct md_ctx *mc, int left_x, int max_w,
                      int size, int base_weight, int base_style,
                      uint32_t base_fg, const char *s, int slen)
{
    const struct font *f = font_get(size, base_weight, base_style);
    int lh = f->h + 4;
    int x = left_x;
    int i = 0;

    if (max_w < f->w * 4)
        max_w = f->w * 4;

    while (i < slen) {
        /* Inline `code` pill */
        if (s[i] == '`') {
            int j = i + 1;
            while (j < slen && s[j] != '`')
                j++;
            if (j < slen) {
                int clen = j - (i + 1);
                int cw = clen * f->w + 8;
                if (x > left_x && x + cw > left_x + max_w) {
                    x = left_x;
                    mc->cur_y += lh;
                }
                md_draw_span(mc, x + 3, mc->cur_y - mc->scroll_y,
                             size, FONT_REGULAR, FONT_ROMAN, base_fg,
                             s + i + 1, clen, 1, 0);
                x += cw + 2;
                i = j + 1;
                continue;
            }
        }

        /* Link [label](url) */
        if (s[i] == '[') {
            int rb = i + 1;
            while (rb < slen && s[rb] != ']')
                rb++;
            if (rb + 1 < slen && s[rb] == ']' && s[rb + 1] == '(') {
                int rp = rb + 2;
                while (rp < slen && s[rp] != ')')
                    rp++;
                if (rp < slen) {
                    int llen = rb - (i + 1);
                    int lw = llen * f->w;
                    if (x > left_x && x + lw > left_x + max_w) {
                        x = left_x;
                        mc->cur_y += lh;
                    }
                    md_draw_span(mc, x, mc->cur_y - mc->scroll_y,
                                 size, base_weight, base_style, base_fg,
                                 s + i + 1, llen, 0, 1);
                    x += lw;
                    i = rp + 1;
                    continue;
                }
            }
        }

        /* **bold** or __bold__ */
        if (i + 3 < slen &&
            ((s[i] == '*' && s[i + 1] == '*') ||
             (s[i] == '_' && s[i + 1] == '_'))) {
            char m = s[i];
            int j = i + 2;
            while (j + 1 < slen && !(s[j] == m && s[j + 1] == m))
                j++;
            if (j + 1 < slen) {
                int blen = j - (i + 2);
                int k = 0;
                while (k < blen) {
                    int avail = (left_x + max_w - x) / f->w;
                    int take;
                    if (avail <= 0) {
                        x = left_x;
                        mc->cur_y += lh;
                        avail = max_w / f->w;
                    }
                    take = blen - k;
                    if (take > avail) {
                        int sp = avail;
                        while (sp > 0 && s[i + 2 + k + sp - 1] != ' ')
                            sp--;
                        take = sp > 0 ? sp : avail;
                    }
                    md_draw_span(mc, x, mc->cur_y - mc->scroll_y,
                                 size, FONT_BOLD, base_style, base_fg,
                                 s + i + 2 + k, take, 0, 0);
                    x += take * f->w;
                    k += take;
                    if (k < blen) {
                        x = left_x;
                        mc->cur_y += lh;
                    }
                }
                i = j + 2;
                continue;
            }
        }

        /* *italic* or _italic_ (avoid mid-identifier underscores) */
        if (i + 2 < slen &&
            (s[i] == '*' ||
             (s[i] == '_' &&
              (i == 0 || !((s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
                           (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                           (s[i - 1] >= '0' && s[i - 1] <= '9'))))) &&
            s[i + 1] != ' ') {
            char m = s[i];
            int j = i + 1;
            while (j < slen && s[j] != m)
                j++;
            if (j < slen) {
                int ilen = j - (i + 1);
                int k = 0;
                while (k < ilen) {
                    int avail = (left_x + max_w - x) / f->w;
                    int take;
                    if (avail <= 0) {
                        x = left_x;
                        mc->cur_y += lh;
                        avail = max_w / f->w;
                    }
                    take = ilen - k;
                    if (take > avail)
                        take = avail;
                    md_draw_span(mc, x, mc->cur_y - mc->scroll_y,
                                 size, base_weight, FONT_ITALIC, base_fg,
                                 s + i + 1 + k, take, 0, 0);
                    x += take * f->w;
                    k += take;
                    if (k < ilen) {
                        x = left_x;
                        mc->cur_y += lh;
                    }
                }
                i = j + 1;
                continue;
            }
        }

        /* Plain word / whitespace run */
        {
            int end = i + 1;
            int is_url = 0;

            if (s[i] == ' ') {
                while (end < slen && s[end] == ' ')
                    end++;
                if (x > left_x && x + (end - i) * f->w <= left_x + max_w) {
                    x += (end - i) * f->w;
                } else if (x > left_x) {
                    x = left_x;
                    mc->cur_y += lh;
                }
                i = end;
                continue;
            }

            if ((i + 7 <= slen && strncmp(s + i, "http://", 7) == 0) ||
                (i + 8 <= slen && strncmp(s + i, "https://", 8) == 0))
                is_url = 1;

            while (end < slen && s[end] != ' ' && s[end] != '`' &&
                   s[end] != '[' && s[end] != '*') {
                end++;
            }
            while (end > i) {
                int run = end - i;
                int avail = (left_x + max_w - x) / f->w;
                if (avail <= 0) {
                    x = left_x;
                    mc->cur_y += lh;
                    avail = max_w / f->w;
                }
                int take = run <= avail ? run : avail;
                if (run > avail && x > left_x && run <= max_w / f->w) {
                    x = left_x;
                    mc->cur_y += lh;
                    avail = max_w / f->w;
                    take = run <= avail ? run : avail;
                }
                md_draw_span(mc, x, mc->cur_y - mc->scroll_y,
                             size, base_weight, base_style, base_fg,
                             s + i, take, 0, is_url);
                x += take * f->w;
                i += take;
            }
        }
    }
    mc->cur_y += lh;
}

static int is_hr_line(const char *s, int len)
{
    char ch = 0;
    int cnt = 0;

    for (int i = 0; i < len; i++) {
        if (s[i] == ' ' || s[i] == '\t')
            continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_')
            return 0;
        if (!ch)
            ch = s[i];
        else if (s[i] != ch)
            return 0;
        cnt++;
    }
    return cnt >= 3;
}

static void md_render_pass(struct md_ctx *mc, const gui_editor *e)
{
    int pad_x = 16;
    int left_x = mc->view.x + pad_x;
    int max_w = mc->view.w - pad_x * 2 - 12;
    int i = 0;

    if (max_w < 80)
        max_w = 80;
    mc->cur_y = mc->view.y + 12;

    while (i < e->nlines) {
        const char *s = e->lines[i].text;
        int len = e->lines[i].len;

        /* Blank line */
        if (len == 0) {
            mc->cur_y += 10;
            i++;
            continue;
        }

        /* Fenced ``` code block */
        if (len >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
            char lang[24];
            int llen = 0;
            int start = i + 1;
            int end = start;
            int card_pad = 10;
            int cols_fit = (max_w - card_pad * 2) / GUI_FONT_W;
            int visual_lines = 0;
            int card_y = mc->cur_y;
            int card_h;

            if (cols_fit < 8)
                cols_fit = 8;
            for (int k = 3; k < len && llen + 1 < (int)sizeof(lang); k++) {
                if (s[k] != ' ')
                    lang[llen++] = s[k];
            }
            lang[llen] = '\0';

            while (end < e->nlines) {
                if (e->lines[end].len >= 3 &&
                    strncmp(e->lines[end].text, "```", 3) == 0)
                    break;
                int v = (e->lines[end].len + cols_fit - 1) / cols_fit;
                visual_lines += v > 0 ? v : 1;
                end++;
            }
            if (visual_lines == 0)
                visual_lines = 1;
            card_h = card_pad * 2 + visual_lines * (GUI_FONT_H + 2);

            if (mc->draw) {
                gui_rc card = gui_mkrc(left_x, card_y - mc->scroll_y,
                                       max_w, card_h);
                gui_fill_round_rect(mc->win, card, 7, GUI_SUNKEN);
                gui_stroke_round_rect(mc->win, card, 7, 1, GUI_EDGE);
                if (lang[0]) {
                    int lw = (int)strlen(lang) * 6 + 10;
                    gui_draw_text_aa(mc->win, left_x + max_w - lw - 4,
                                     card_y - mc->scroll_y + 6, lang,
                                     FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                                     GUI_TEXT_DIM, GUI_TRANSPARENT);
                }
                int cy = card_y - mc->scroll_y + card_pad;
                for (int ln = start; ln < end; ln++) {
                    const char *cs = e->lines[ln].text;
                    int clen = e->lines[ln].len;
                    int off = 0;
                    do {
                        int take = clen - off;
                        if (take > cols_fit)
                            take = cols_fit;
                        if (cy + GUI_FONT_H >= mc->view.y &&
                            cy <= mc->view.y + mc->view.h)
                            gui_text_n(mc->win, left_x + card_pad, cy,
                                       cs + off, take, GUI_OK,
                                       GUI_TRANSPARENT);
                        cy += GUI_FONT_H + 2;
                        off += take;
                    } while (off < clen);
                }
            }
            mc->cur_y += card_h + 8;
            i = (end < e->nlines) ? end + 1 : end;
            continue;
        }

        /* Headings #, ##, ### */
        if (s[0] == '#') {
            int lvl = 0;
            while (lvl < len && lvl < 4 && s[lvl] == '#')
                lvl++;
            if (lvl < len && s[lvl] == ' ') {
                int sz = (lvl == 1) ? FONT_HUGE
                       : (lvl == 2) ? FONT_LARGE : FONT_BODY;
                uint32_t col = (lvl == 1) ? GUI_TEXT : GUI_ACCENT;
                if (i > 0)
                    mc->cur_y += 4;
                md_inline(mc, left_x, max_w, sz, FONT_BOLD, FONT_ROMAN,
                          col, s + lvl + 1, len - (lvl + 1));
                if (lvl == 1) {
                    if (mc->draw)
                        gui_rect(mc->win, left_x,
                                 mc->cur_y - mc->scroll_y - 2,
                                 max_w, 1, GUI_EDGE);
                    mc->cur_y += 6;
                } else {
                    mc->cur_y += 2;
                }
                i++;
                continue;
            }
        }

        /* Horizontal divider --- / *** / ___ */
        if (is_hr_line(s, len)) {
            if (mc->draw)
                gui_rect(mc->win, left_x, mc->cur_y - mc->scroll_y + 7,
                         max_w, 1, GUI_EDGE);
            mc->cur_y += 16;
            i++;
            continue;
        }

        /* Blockquote > text */
        if (s[0] == '>') {
            const char *q = s + 1;
            int qlen = len - 1;
            int start_y = mc->cur_y;
            if (qlen > 0 && *q == ' ') {
                q++;
                qlen--;
            }
            if (mc->draw) {
                /* Pre-measure quote height for clean background bar */
                struct md_ctx dry = *mc;
                dry.draw = 0;
                dry.cur_y = start_y;
                md_inline(&dry, left_x + 14, max_w - 18, FONT_BODY,
                          FONT_REGULAR, FONT_ITALIC, GUI_TEXT_DIM, q, qlen);
                int qh = dry.cur_y - start_y;
                gui_fill_rect_alpha(mc->win, left_x, start_y - mc->scroll_y,
                                    max_w, qh, GUI_ACCENT, 22);
                gui_rect(mc->win, left_x, start_y - mc->scroll_y,
                         3, qh, GUI_ACCENT);
            }
            md_inline(mc, left_x + 14, max_w - 18, FONT_BODY,
                      FONT_REGULAR, FONT_ITALIC, GUI_TEXT_DIM, q, qlen);
            mc->cur_y += 4;
            i++;
            continue;
        }

        /* Bullet list - / * / + (and optional [ ] / [x] task box) */
        {
            int lead = 0;
            while (lead < len && s[lead] == ' ')
                lead++;
            if (lead + 1 < len &&
                (s[lead] == '-' || s[lead] == '*' || s[lead] == '+') &&
                s[lead + 1] == ' ') {
                int indent = (lead / 2) * 16;
                int bx = left_x + indent;
                int item_y = mc->cur_y - mc->scroll_y;
                const char *body = s + lead + 2;
                int blen = len - (lead + 2);
                int is_task = 0, checked = 0;

                if (blen >= 4 && body[0] == '[' &&
                    (body[1] == ' ' || body[1] == 'x' || body[1] == 'X') &&
                    body[2] == ']' && body[3] == ' ') {
                    is_task = 1;
                    checked = (body[1] != ' ');
                    body += 4;
                    blen -= 4;
                }

                if (mc->draw && item_y + 16 >= mc->view.y &&
                    item_y <= mc->view.y + mc->view.h) {
                    if (is_task) {
                        gui_rc cb = gui_mkrc(bx + 2, item_y + 2, 12, 12);
                        gui_round_rect(mc->win, cb, 3,
                                       checked ? GUI_ACCENT : GUI_SUNKEN);
                        gui_round_frame(mc->win, cb, 3, GUI_EDGE);
                        if (checked)
                            gui_icon(mc->win, cb.x + 1, cb.y + 1, 10,
                                     GUI_ICON_CHECK, GUI_WHITE);
                    } else if (indent > 0) {
                        gui_circle(mc->win, bx + 7, item_y + 8, 3, GUI_ACCENT);
                    } else {
                        gui_disc(mc->win, bx + 7, item_y + 8, 3, GUI_ACCENT);
                    }
                }
                md_inline(mc, bx + 20, max_w - indent - 20, FONT_BODY,
                          FONT_REGULAR, FONT_ROMAN, GUI_TEXT, body, blen);
                i++;
                continue;
            }
        }

        /* Normal paragraph line */
        md_inline(mc, left_x, max_w, FONT_BODY, FONT_REGULAR, FONT_ROMAN,
                  GUI_TEXT, s, len);
        i++;
    }
}

static void draw_markdown_preview(gui_window *win, gui_rc r,
                                  const gui_editor *e, int *scroll_y,
                                  gui_ui *ui)
{
    struct md_ctx mc;
    int total_h, max_scroll;
    gui_rc sb_rc, content_rc;

    gui_round_rect(win, r, 7, GUI_SURFACE_ALT);
    gui_round_frame(win, r, 7, GUI_EDGE);

    mc.win = win;
    mc.view = gui_mkrc(r.x + 2, r.y + 2, r.w - 16, r.h - 4);
    mc.draw = 0;
    mc.scroll_y = 0;
    mc.cur_y = 0;
    md_render_pass(&mc, e);
    total_h = (mc.cur_y - mc.view.y) + 12;

    max_scroll = total_h - mc.view.h;
    if (max_scroll < 0)
        max_scroll = 0;

    if (ui->wheel && gui_hit(r, ui->mx, ui->my)) {
        *scroll_y -= ui->wheel * 36;
        ui->wheel = 0;
    }
    if (*scroll_y < 0)
        *scroll_y = 0;
    if (*scroll_y > max_scroll)
        *scroll_y = max_scroll;

    content_rc = mc.view;
    if (total_h > mc.view.h) {
        sb_rc = gui_mkrc(r.x + r.w - 13, r.y + 3, 10, r.h - 6);
        gui_scrollbar(win, sb_rc, total_h, mc.view.h, scroll_y, ui);
    } else {
        *scroll_y = 0;
    }

    mc.draw = 1;
    mc.scroll_y = *scroll_y;
    mc.view = content_rc;
    gui_push_clip(win, content_rc);
    md_render_pass(&mc, e);
    gui_pop_clip(win);
}

/* ---- sidebar & UI ---------------------------------------------------- */

static const char *note_item_cb(void *ctx, int index)
{
    static char label[MAX_NAME_LEN + 6];

    (void)ctx;
    if (index < 0 || index >= nnotes)
        return "";
    if (index == cur_idx && ed->modified)
        snprintf(label, sizeof(label), "%s *", notes[index].name);
    else
        snprintf(label, sizeof(label), "%s", notes[index].name);
    return label;
}

static uint32_t note_tint_cb(void *ctx, int index)
{
    (void)ctx;
    if (index < 0 || index >= nnotes)
        return GUI_TEXT;
    if (notes[index].external)
        return GUI_ACCENT;
    return GUI_TEXT;
}

static void execute_pending(int save_first)
{
    enum pending_action act = pending_act;
    int sel = pending_sel_idx;

    pending_act = PEND_NONE;
    pending_sel_idx = -1;
    if (save_first && save_current_note() < 0)
        return;
    if (act == PEND_NEW)
        create_new_note();
    else if (act == PEND_SELECT && sel >= 0 && sel < nnotes)
        open_note_path(notes[sel].path);
    else if (act == PEND_DELETE)
        delete_current_note();
}

static void paint(gui_window *win, gui_ui *ui)
{
    int W = win->w, H = win->h;
    int top_h = 46, bot_h = 30, pad = 8;
    int side_w = W >= 760 ? 186 : (W >= 560 ? 150 : 120);
    int main_x = side_w + pad;
    int main_y = top_h + pad;
    int main_w = W - side_w - pad * 2;
    int main_h = H - top_h - bot_h - pad * 2;
    int x;

    if (main_w < 180)
        main_w = 180;
    if (main_h < 80)
        main_h = 80;

    gui_clear(win, GUI_SURFACE);

    /* ---- Top Toolbar ---- */
    gui_hgradient(win, gui_mkrc(0, 0, W, top_h),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 26), GUI_PANEL);
    gui_rect(win, 0, top_h - 1, W, 1, GUI_EDGE);
    gui_icon(win, 10, 11, 22, GUI_ICON_FILES, GUI_ACCENT);
    gui_draw_text_aa(win, 38, 12, "Notes", FONT_LARGE, FONT_BOLD, FONT_ROMAN,
                     GUI_TEXT, GUI_TRANSPARENT);

    {
        int compact = (W < 790);
        int bw_new = compact ? 50 : 64;
        int bw_save = compact ? 48 : 64;
        int bw_del = compact ? 44 : 68;
        int bw_in = compact ? 54 : 112;
        int bw_to = compact ? 54 : 118;
        int gap = compact ? 4 : 6;

        x = compact ? 112 : 122;
        if (gui_button(win, gui_mkrc(x, 9, bw_new, 28),
                       compact ? "+New" : "+ New", ui)) {
            if (ed->modified) {
                pending_act = PEND_NEW;
                snprintf(status_msg, sizeof(status_msg),
                         "Save changes to %s before creating note?", cur_name);
            } else {
                create_new_note();
            }
            need_repaint = 1;
        }
        x += bw_new + gap;
        if (gui_button_ex(win, gui_mkrc(x, 9, bw_save, 28), "Save", ui, 1,
                          ed->modified)) {
            save_current_note();
            need_repaint = 1;
        }
        x += bw_save + gap;
        if (gui_button_ex(win, gui_mkrc(x, 9, bw_del, 28),
                          compact ? "Del" : "Delete", ui,
                          cur_path[0] != '\0', pending_act == PEND_DELETE)) {
            pending_act = PEND_DELETE;
            snprintf(status_msg, sizeof(status_msg), "Delete %s?", cur_name);
            need_repaint = 1;
        }
        x += bw_del + gap;
        if (gui_button(win, gui_mkrc(x, 9, bw_in, 28),
                       compact ? "In Tln" : "Insert Talon", ui)) {
            insert_talon_into_editor();
            need_repaint = 1;
        }
        x += bw_in + gap;
        if (gui_button(win, gui_mkrc(x, 9, bw_to, 28),
                       compact ? "To Tln" : "Send to Talon", ui)) {
            send_note_to_talon();
            need_repaint = 1;
        }
    }

    /* View mode toggle [Edit] [Split] [Preview] on top-right */
    {
        static const char *const mnames[3] = { "Edit", "Split", "Preview" };
        static const char *const mshort[3] = { "Ed", "Sp", "Pv" };
        int narrow = (W < 600);
        int mw = narrow ? 38 : 60;
        int mx = W - 10 - mw * 3 - 8;
        for (int m = 0; m < 3; m++) {
            if (gui_button_ex(win, gui_mkrc(mx + m * (mw + 4), 9, mw, 28),
                              narrow ? mshort[m] : mnames[m],
                              ui, 1, (int)view_mode == m)) {
                view_mode = (enum view_mode)m;
                ed->focus = (view_mode != VIEW_PREVIEW);
                need_repaint = 1;
            }
        }
    }

    /* ---- Left Note Sidebar ---- */
    gui_rect(win, 0, top_h, side_w, H - top_h - bot_h,
             gui_mix(GUI_PANEL, GUI_BLACK, 14));
    gui_rect(win, side_w - 1, top_h, 1, H - top_h - bot_h, GUI_EDGE);
    gui_draw_text_aa_run(win, 10, top_h + 8, notes_dir, -1,
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT, side_w - 20, 1);

    note_list.row_h = 24;
    note_list.item = note_item_cb;
    note_list.tint = note_tint_cb;
    note_list.ctx = 0;
    {
        gui_rc sb_list_rc = gui_mkrc(8, top_h + 28, side_w - 16,
                                     H - top_h - bot_h - 36);
        int prev_sel = note_list.sel;
        int act = gui_listbox(win, sb_list_rc, &note_list, ui);
        int chosen = (act >= 0) ? act : note_list.sel;

        if (chosen != prev_sel && chosen >= 0 && chosen < nnotes &&
            chosen != cur_idx) {
            if (ed->modified) {
                pending_act = PEND_SELECT;
                pending_sel_idx = chosen;
                note_list.sel = cur_idx;
                snprintf(status_msg, sizeof(status_msg),
                         "Save %s before switching to %s?",
                         cur_name, notes[chosen].name);
            } else {
                open_note_path(notes[chosen].path);
            }
            need_repaint = 1;
        }
    }

    /* ---- Main Editor / Split / Preview Canvas ---- */
    if (view_mode == VIEW_EDIT) {
        gui_rc ed_rc = gui_mkrc(main_x, main_y, main_w, main_h);
        if (gui_editor_event(ed, ed_rc, ui))
            need_repaint = 1;
        gui_editor_draw(win, ed_rc, ed, ui);
    } else if (view_mode == VIEW_PREVIEW) {
        gui_rc pv_rc = gui_mkrc(main_x, main_y, main_w, main_h);
        draw_markdown_preview(win, pv_rc, ed, &preview_scroll, ui);
    } else {
        int half = (main_w - pad) / 2;
        gui_rc ed_rc = gui_mkrc(main_x, main_y, half, main_h);
        gui_rc pv_rc = gui_mkrc(main_x + half + pad, main_y,
                                main_w - half - pad, main_h);
        if (gui_editor_event(ed, ed_rc, ui))
            need_repaint = 1;
        gui_editor_draw(win, ed_rc, ed, ui);
        draw_markdown_preview(win, pv_rc, ed, &preview_scroll, ui);
    }

    /* ---- Bottom Status & Confirmation Bar ---- */
    gui_rect(win, 0, H - bot_h, W, bot_h, GUI_PANEL);
    gui_rect(win, 0, H - bot_h, W, 1, GUI_EDGE);

    if (pending_act != PEND_NONE) {
        gui_push_clip_xy(win, 10, H - bot_h, W - 240, bot_h);
        gui_text(win, 10, H - bot_h + 7, status_msg, GUI_WARN,
                 GUI_TRANSPARENT);
        gui_pop_clip(win);

        if (pending_act == PEND_DELETE) {
            if (gui_button(win, gui_mkrc(W - 150, H - bot_h + 3, 66, 24),
                           "Yes", ui)) {
                execute_pending(0);
                need_repaint = 1;
            }
            if (gui_button(win, gui_mkrc(W - 76, H - bot_h + 3, 66, 24),
                           "No", ui)) {
                pending_act = PEND_NONE;
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
                need_repaint = 1;
            }
        } else {
            if (gui_button(win, gui_mkrc(W - 224, H - bot_h + 3, 66, 24),
                           "Save", ui)) {
                execute_pending(1);
                need_repaint = 1;
            }
            if (gui_button(win, gui_mkrc(W - 150, H - bot_h + 3, 70, 24),
                           "Discard", ui)) {
                execute_pending(0);
                need_repaint = 1;
            }
            if (gui_button(win, gui_mkrc(W - 72, H - bot_h + 3, 64, 24),
                           "Cancel", ui)) {
                pending_act = PEND_NONE;
                pending_sel_idx = -1;
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
                need_repaint = 1;
            }
        }
    } else {
        char info_s[64];
        snprintf(info_s, sizeof(info_s), "Ln %d Col %d  %s%s",
                 ed->cursor_line + 1, ed->cursor_col + 1,
                 cur_name[0] ? cur_name : "-", ed->modified ? "*" : "");
        gui_push_clip_xy(win, 10, H - bot_h, W - gui_text_w(info_s) - 24,
                         bot_h);
        gui_text(win, 10, H - bot_h + 7, status_msg, GUI_TEXT_DIM,
                 GUI_TRANSPARENT);
        gui_pop_clip(win);
        gui_text(win, W - gui_text_w(info_s) - 10, H - bot_h + 7,
                 info_s, GUI_TEXT, GUI_TRANSPARENT);
    }
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    const char *cwd_arg = 0;
    const char *file_arg = 0;
    int sw = 1024, sh = 768, win_w = 880, win_h = 560;
    unsigned long last_blink = 0;

    argc = krec_strip_cwd(argc, argv, &cwd_arg);
    if (cwd_arg && cwd_arg[0])
        snprintf(g_cwd, sizeof(g_cwd), "%s", cwd_arg);
    if (argc > 1)
        file_arg = argv[1];

    ed = gui_editor_create();
    if (!ed) {
        printf("notes: out of memory\n");
        return 1;
    }

    init_notes_dir();
    memset(&note_list, 0, sizeof(note_list));
    note_list.sel = -1;

    if (file_arg && file_arg[0]) {
        char fpath[MAX_PATH_LEN];
        resolve_path(file_arg, fpath, sizeof(fpath));
        open_note_path(fpath);
    } else {
        reload_note_list(0);
        if (nnotes > 0) {
            open_note_path(notes[0].path);
        } else {
            char init_path[MAX_PATH_LEN];
            static const char *welcome =
                "# KestrelOS Avionics Notes\n\n"
                "Welcome to your Markdown scratchpad and flight log.\n\n"
                "## Quick Controls\n\n"
                "- **Ctrl+S** or **Save** writes to disk\n"
                "- **Insert Talon** / **Send to Talon** exchanges parcels\n"
                "- **Edit / Split / Preview** switches view modes\n\n"
                "> Tip: `inspect -r procs --pin` sends live tables straight to Talon.\n\n"
                "---\n\n"
                "```c\n"
                "// KestrelOS Avionics Scratchpad\n"
                "int status = 0;\n"
                "```\n";
            snprintf(init_path, sizeof(init_path), "%s/flight-log.md",
                     notes_dir);
            snprintf(cur_path, sizeof(cur_path), "%s", init_path);
            snprintf(cur_name, sizeof(cur_name), "flight-log.md");
            gui_editor_set_text(ed, welcome);
            ed->modified = 0;
            reload_note_list(init_path);
            snprintf(status_msg, sizeof(status_msg),
                     "Scratchpad ready (%s)", notes_dir);
        }
    }

    if (gui_screen(&sw, &sh) == 0) {
        if (win_w > sw - 40)
            win_w = sw - 40;
        if (win_h > sh - 80)
            win_h = sh - 80;
        if (win_w < 540)
            win_w = 540;
        if (win_h < 380)
            win_h = 380;
    }

    win = gui_open("Avionics Notes", 80, 60, win_w, win_h, 0);
    if (!win) {
        printf("notes: no window manager available\n");
        gui_editor_destroy(ed);
        return 1;
    }

    memset(&ui, 0, sizeof(ui));
    while (win->open) {
        int got = gui_next_event(win, &ev, dirty ? 0 : 100);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY) {
                if (ev.key == 19) { /* Ctrl+S: Save */
                    save_current_note();
                    ui.key = 0;
                } else if (ev.key == 16) { /* Ctrl+P: cycle view mode */
                    view_mode = (enum view_mode)(((int)view_mode + 1) % 3);
                    ed->focus = (view_mode != VIEW_PREVIEW);
                    ui.key = 0;
                } else if (ev.key == 14) { /* Ctrl+N: New note */
                    if (ed->modified) {
                        pending_act = PEND_NEW;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Save changes to %s before creating note?",
                                 cur_name);
                    } else {
                        create_new_note();
                    }
                    ui.key = 0;
                }
            }
            dirty = 1;
        }

        if (ed->focus && view_mode != VIEW_PREVIEW) {
            unsigned long phase = uptime_ms() / 500UL;
            if (phase != last_blink) {
                last_blink = phase;
                dirty = 1;
            }
        }

        if (!dirty)
            continue;
        need_repaint = 0;
        paint(win, &ui);
        if (gui_flush(win) != 0)
            break;
        dirty = need_repaint;
    }

    gui_close(win);
    gui_editor_destroy(ed);
    return 0;
}
