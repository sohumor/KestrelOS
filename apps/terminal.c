/* terminal.c - a graphical terminal emulator for KestrelOS.
 *
 * A resizable ANSI-subset terminal emulator in a libgui window backed by
 * a persistent interactive /bin/sh process over two kernel pipes via
 * pipe_() and spawn_caps().
 *
 * Features:
 *   - non-blocking output pumping via read_nb() so the UI stays fluid;
 *   - CSI cursor motion, erase, SGR colours, 200 lines of scrollback;
 *   - PgUp/PgDn, scrollbar, and KEV_MOUSE_WHEEL scrollback navigation;
 *   - KEV_RESIZE dynamic grid recalculation (columns and rows);
 *   - Ctrl+C -> SIGINT to foreground child process(es), or 0x03 at prompt;
 *   - Mouse drag text selection + Talon clipboard copy/paste via
 *     parcel_put()/parcel_get() (Ctrl+V / Ctrl+Shift+V paste, Ctrl+C /
 *     Ctrl+Shift+C copy when selection is active, plus Copy/Paste bar).
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* ------------------------------------------------------------ geometry */

#define DEF_COLS    80
#define DEF_ROWS    24
#define MAX_COLS    256
#define MAX_ROWS    128
#define SCROLLBACK  200
#define TOTAL       (MAX_ROWS + SCROLLBACK)

#define PAD         6
#define BAR_W       14
#define STATUS_H    24
#define DEF_WIN_W   (PAD + DEF_COLS * GUI_FONT_W + 4 + BAR_W + PAD)
#define DEF_WIN_H   (PAD + DEF_ROWS * GUI_FONT_H + PAD + STATUS_H)

#define MAX_PATH    256
#define CLIP_MAX    8192

/* ANSI colours, in the same slate family as the desktop theme. */
static const uint32_t palette[16] = {
    0x18202AU, 0xC4706AU, 0x7FB07FU, 0xD4A45CU,
    0x5B87D0U, 0xAE7FC0U, 0x5BA3B0U, 0xC2CBD5U,
    0x38424EU, 0xE08A82U, 0x9CCC9CU, 0xEFC178U,
    0x7FA8E8U, 0xC79AD8U, 0x79C4D0U, 0xEFF3F7U
};

#define DEF_FG 7
#define DEF_BG 0

struct cell {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
};

/* ---------------------------------------------------------------- state */

static struct cell grid[TOTAL][MAX_COLS];
static int term_cols = DEF_COLS;
static int term_rows = DEF_ROWS;
static int win_w = DEF_WIN_W;
static int win_h = DEF_WIN_H;

static int base;                /* monotonic ring index of screen row 0  */
static int back_count;          /* scrollback rows currently stored      */
static int view;                /* rows scrolled back, 0 = live          */
static int cx, cy;              /* cursor, screen coordinates            */
static int cur_fg = DEF_FG, cur_bg = DEF_BG;
static int bold, reverse;
static int cursor_on = 1;
static int saved_cx, saved_cy;
static int dirty = 1;
static int focused = 1;

static int esc_state;           /* 0 text, 1 ESC, 2 CSI, 3/4 OSC         */
static char csi[32];
static int csi_len;

static gui_window *g_win;
static gui_ui g_ui;
static char cwd[MAX_PATH];
static int want_exit;

static int shell_in = -1;       /* write end: keystrokes to /bin/sh      */
static int shell_out = -1;      /* read end: /bin/sh stdout + stderr     */
static int shell_pid = -1;
static int fg_pid = -1;
static unsigned long last_proc_poll;

static char status_msg[80];
static unsigned long status_until;

/* Mouse text selection in monotonic ring coordinates. */
static int sel_active;
static int sel_dragging;
static int sel_a_row, sel_a_col;
static int sel_b_row, sel_b_col;

static char clip_buf[CLIP_MAX];

/* --------------------------------------------------------- grid helpers */

static struct cell *row_at(int ring)
{
    return grid[((ring % TOTAL) + TOTAL) % TOTAL];
}

static struct cell *screen_row(int r, int offset)
{
    return row_at(base - offset + r);
}

static void clear_cells(struct cell *row, int from, int to)
{
    int i;

    for (i = from; i < to && i < MAX_COLS; i++) {
        row[i].ch = ' ';
        row[i].fg = (unsigned char)DEF_FG;
        row[i].bg = (unsigned char)cur_bg;
    }
}

static void set_toast(const char *msg)
{
    snprintf(status_msg, sizeof(status_msg), "%s", msg);
    status_until = uptime_ms() + 1600;
    dirty = 1;
}

static void clamp_scrollback(void)
{
    int max_back = TOTAL - term_rows;

    if (max_back < 0)
        max_back = 0;
    if (back_count > max_back)
        back_count = max_back;
    if (view > back_count)
        view = back_count;
    if (view < 0)
        view = 0;
}

static void scroll_up_one(void)
{
    base++;
    if (back_count < TOTAL - term_rows)
        back_count++;
    clear_cells(screen_row(term_rows - 1, 0), 0, MAX_COLS);
    if (view > 0 && view < back_count)
        view++;
    if (sel_active && (sel_a_row < base - back_count ||
                       sel_b_row < base - back_count))
        sel_active = 0;
    dirty = 1;
}

static void line_feed(void)
{
    cy++;
    if (cy >= term_rows) {
        cy = term_rows - 1;
        scroll_up_one();
    }
}

static void put_glyph(unsigned char c)
{
    struct cell *row;
    int fg = bold ? (cur_fg | 8) : cur_fg;
    int bg = cur_bg;

    if (cx >= term_cols) {
        cx = 0;
        line_feed();
    }
    if (reverse) {
        int t = fg;

        fg = bg;
        bg = t;
    }
    row = screen_row(cy, 0);
    row[cx].ch = c;
    row[cx].fg = (unsigned char)(fg & 15);
    row[cx].bg = (unsigned char)(bg & 15);
    cx++;
    dirty = 1;
}

static void erase_screen(int mode)
{
    int r;

    if (mode == 2) {
        for (r = 0; r < term_rows; r++)
            clear_cells(screen_row(r, 0), 0, term_cols);
    } else if (mode == 1) {
        for (r = 0; r < cy; r++)
            clear_cells(screen_row(r, 0), 0, term_cols);
        clear_cells(screen_row(cy, 0), 0, cx + 1);
    } else {
        clear_cells(screen_row(cy, 0), cx, term_cols);
        for (r = cy + 1; r < term_rows; r++)
            clear_cells(screen_row(r, 0), 0, term_cols);
    }
    dirty = 1;
}

static void erase_line(int mode)
{
    struct cell *row = screen_row(cy, 0);

    if (mode == 1)
        clear_cells(row, 0, cx + 1);
    else if (mode == 2)
        clear_cells(row, 0, term_cols);
    else
        clear_cells(row, cx, term_cols);
    dirty = 1;
}

/* ------------------------------------------------------- escape parsing */

static void sgr(const int *p, int np)
{
    int i;

    if (np == 0) {
        cur_fg = DEF_FG;
        cur_bg = DEF_BG;
        bold = 0;
        reverse = 0;
        return;
    }
    for (i = 0; i < np; i++) {
        int v = p[i];

        if (v == 0) {
            cur_fg = DEF_FG;
            cur_bg = DEF_BG;
            bold = 0;
            reverse = 0;
        } else if (v == 1) {
            bold = 1;
        } else if (v == 7) {
            reverse = 1;
        } else if (v == 22) {
            bold = 0;
        } else if (v == 27) {
            reverse = 0;
        } else if (v >= 30 && v <= 37) {
            cur_fg = v - 30;
        } else if (v == 39) {
            cur_fg = DEF_FG;
        } else if (v >= 40 && v <= 47) {
            cur_bg = v - 40;
        } else if (v == 49) {
            cur_bg = DEF_BG;
        } else if (v >= 90 && v <= 97) {
            cur_fg = (v - 90) | 8;
        } else if (v >= 100 && v <= 107) {
            cur_bg = (v - 100) | 8;
        }
    }
}

static void clamp_cursor(void)
{
    if (cx < 0)
        cx = 0;
    if (cx > term_cols - 1)
        cx = term_cols - 1;
    if (cy < 0)
        cy = 0;
    if (cy > term_rows - 1)
        cy = term_rows - 1;
}

static void do_csi(unsigned char final)
{
    int p[8], np = 0, priv = 0;
    const char *s = csi;
    int v = 0, seen = 0;

    csi[csi_len] = '\0';
    if (*s == '?') {
        priv = 1;
        s++;
    }
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            seen = 1;
            continue;
        }
        if (np < 8)
            p[np++] = seen ? v : 0;
        v = 0;
        seen = 0;
        if (*s != ';')
            break;
    }
    if (np == 0)
        p[np++] = 0;

#define ARG(i) ((i) < np ? p[i] : 0)
#define ARG1(i) (ARG(i) < 1 ? 1 : ARG(i))

    switch (final) {
    case 'A': cy -= ARG1(0); break;
    case 'B': cy += ARG1(0); break;
    case 'C': cx += ARG1(0); break;
    case 'D': cx -= ARG1(0); break;
    case 'E': cy += ARG1(0); cx = 0; break;
    case 'F': cy -= ARG1(0); cx = 0; break;
    case 'G': cx = ARG1(0) - 1; break;
    case 'd': cy = ARG1(0) - 1; break;
    case 'H':
    case 'f':
        cy = ARG1(0) - 1;
        cx = ARG1(1) - 1;
        break;
    case 'J': erase_screen(ARG(0)); break;
    case 'K': erase_line(ARG(0)); break;
    case 'm': sgr(p, np); break;
    case 'h':
        if (priv && ARG(0) == 25)
            cursor_on = 1;
        break;
    case 'l':
        if (priv && ARG(0) == 25)
            cursor_on = 0;
        break;
    case 's':
        saved_cx = cx;
        saved_cy = cy;
        break;
    case 'u':
        cx = saved_cx;
        cy = saved_cy;
        break;
    default:
        break;
    }
    clamp_cursor();
    dirty = 1;

#undef ARG
#undef ARG1
}

static void put_byte(unsigned char c)
{
    switch (esc_state) {
    case 1:
        if (c == '[') {
            esc_state = 2;
            csi_len = 0;
        } else if (c == ']') {
            esc_state = 3;      /* OSC: swallow to BEL or ST */
        } else {
            esc_state = 0;
        }
        return;
    case 2:
        if (c >= 0x40 && c <= 0x7E) {
            do_csi(c);
            esc_state = 0;
        } else if (csi_len < (int)sizeof(csi) - 1) {
            csi[csi_len++] = (char)c;
        }
        return;
    case 3:
        if (c == 7)
            esc_state = 0;
        else if (c == 27)
            esc_state = 4;
        return;
    case 4:
        esc_state = 0;
        return;
    default:
        break;
    }

    switch (c) {
    case 27:
        esc_state = 1;
        break;
    case '\n':
        cx = 0;
        line_feed();
        dirty = 1;
        break;
    case '\r':
        cx = 0;
        dirty = 1;
        break;
    case '\b':
        if (cx > 0)
            cx--;
        dirty = 1;
        break;
    case '\t': {
        int n = 8 - (cx % 8);

        while (n-- > 0)
            put_glyph(' ');
        break;
    }
    case 7:
    case 0:
        break;
    default:
        if (c >= 32)
            put_glyph(c);
        break;
    }
}

static void feed(const char *buf, long n)
{
    long i;

    for (i = 0; i < n; i++)
        put_byte((unsigned char)buf[i]);
    if (n > 0)
        view = 0;
}

static void feed_str(const char *s)
{
    feed(s, (long)strlen(s));
}

/* --------------------------------------------------- selection & Talon */

static void selection_bounds(int *r0, int *c0, int *r1, int *c1)
{
    if (sel_a_row < sel_b_row ||
        (sel_a_row == sel_b_row && sel_a_col <= sel_b_col)) {
        *r0 = sel_a_row;
        *c0 = sel_a_col;
        *r1 = sel_b_row;
        *c1 = sel_b_col;
    } else {
        *r0 = sel_b_row;
        *c0 = sel_b_col;
        *r1 = sel_a_row;
        *c1 = sel_a_col;
    }
}

static int cell_is_selected(int abs_row, int col)
{
    int r0, c0, r1, c1;

    if (!sel_active)
        return 0;
    selection_bounds(&r0, &c0, &r1, &c1);
    if (abs_row < r0 || abs_row > r1)
        return 0;
    if (r0 == r1)
        return col >= c0 && col <= c1;
    if (abs_row == r0)
        return col >= c0;
    if (abs_row == r1)
        return col <= c1;
    return 1;
}

static void copy_to_parcel(void)
{
    int r0, c0, r1, c1;
    int r, c;
    int out = 0;
    char msg[64];

    if (sel_active) {
        selection_bounds(&r0, &c0, &r1, &c1);
    } else {
        r0 = base - view;
        c0 = 0;
        r1 = r0 + term_rows - 1;
        c1 = term_cols - 1;
    }

    for (r = r0; r <= r1 && out < CLIP_MAX - 2; r++) {
        const struct cell *row = row_at(r);
        int start = (r == r0) ? c0 : 0;
        int end = (r == r1) ? c1 : (term_cols - 1);

        if (start < 0) start = 0;
        if (end >= term_cols) end = term_cols - 1;
        while (end >= start && (row[end].ch == ' ' || row[end].ch == 0))
            end--;
        for (c = start; c <= end && out < CLIP_MAX - 2; c++) {
            unsigned char ch = row[c].ch;
            clip_buf[out++] = (ch >= 32) ? (char)ch : ' ';
        }
        if (r < r1 && out < CLIP_MAX - 1)
            clip_buf[out++] = '\n';
    }
    while (!sel_active && out > 0 && clip_buf[out - 1] == '\n')
        out--;
    clip_buf[out] = '\0';

    if (out <= 0) {
        set_toast("nothing to copy");
        return;
    }
    if (parcel_put(K_PARCEL_TEXT, "text/plain", "terminal",
                   clip_buf, (uint32_t)out) >= 0) {
        snprintf(msg, sizeof(msg), "copied %d bytes", out);
        set_toast(msg);
    } else {
        set_toast("copy failed");
    }
}

static void write_pasted_bytes(const char *buf, long n)
{
    long i;

    if (shell_in < 0 || n <= 0)
        return;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
            continue;
        write(shell_in, &buf[i], 1);
    }
}

static void paste_from_parcel(void)
{
    struct k_parcel_info info;
    long got = -1;
    int i;
    char msg[64];

    memset(&info, 0, sizeof(info));
    got = parcel_get(0, &info, clip_buf, sizeof(clip_buf) - 1);
    if (got < 0 || info.type != K_PARCEL_TEXT) {
        got = -1;
        for (i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
            if (parcel_list(i, &info) == 0 && info.type == K_PARCEL_TEXT) {
                got = parcel_get(info.id, &info, clip_buf, sizeof(clip_buf) - 1);
                break;
            }
        }
    }
    if (got <= 0) {
        set_toast("clipboard empty");
        return;
    }
    clip_buf[got] = '\0';
    write_pasted_bytes(clip_buf, got);
    snprintf(msg, sizeof(msg), "pasted %ld bytes", got);
    set_toast(msg);
}

/* ----------------------------------------------------- resize & layout */

static void compute_grid_size(int nw, int nh, int *cols_out, int *rows_out)
{
    int avail_w = nw - PAD - 4 - BAR_W - PAD;
    int avail_h = nh - PAD - PAD - STATUS_H;
    int c = avail_w / GUI_FONT_W;
    int r = avail_h / GUI_FONT_H;

    if (c < 16) c = 16;
    if (c > MAX_COLS) c = MAX_COLS;
    if (r < 4) r = 4;
    if (r > MAX_ROWS) r = MAX_ROWS;
    *cols_out = c;
    *rows_out = r;
}

static void handle_resize(int nw, int nh)
{
    int new_cols, new_rows;
    int old_cols = term_cols;
    int old_rows = term_rows;
    int i, r;

    compute_grid_size(nw, nh, &new_cols, &new_rows);
    win_w = nw;
    win_h = nh;

    if (new_cols > old_cols) {
        for (i = 0; i < TOTAL; i++)
            clear_cells(grid[i], old_cols, new_cols);
    }

    if (new_rows < old_rows) {
        if (cy >= new_rows) {
            int drop = cy - (new_rows - 1);
            base += drop;
            back_count += drop;
            cy = new_rows - 1;
        }
    } else if (new_rows > old_rows) {
        int extra = new_rows - old_rows;
        int pull = extra < back_count ? extra : back_count;
        base -= pull;
        back_count -= pull;
        cy += pull;
        for (r = old_rows + pull; r < new_rows; r++)
            clear_cells(row_at(base + r), 0, new_cols);
    }

    term_cols = new_cols;
    term_rows = new_rows;
    clamp_scrollback();
    clamp_cursor();
    sel_active = 0;
    sel_dragging = 0;
    dirty = 1;
}

/* ------------------------------------------------------------ rendering */

static void draw_status(void)
{
    gui_rc r = gui_mkrc(0, win_h - STATUS_H, win_w, STATUS_H);
    char left[96];
    char right[48];
    int right_w, btn_x, btn_y, btn_h, left_limit;

    gui_rect(g_win, r.x, r.y, r.w, r.h, GUI_PANEL);
    gui_rect(g_win, r.x, r.y, r.w, 1, GUI_BORDER);

    if (view > 0)
        snprintf(right, sizeof(right), "-%d/%d", view, back_count);
    else
        snprintf(right, sizeof(right), "/bin/sh");
    right_w = gui_text_w(right);

    btn_h = STATUS_H - 4;
    btn_y = r.y + 2;
    btn_x = win_w - 8 - right_w - 8;
    left_limit = btn_x;

    if (win_w >= 420) {
        gui_rc paste_rc = gui_mkrc(btn_x - 52, btn_y, 50, btn_h);
        gui_rc copy_rc = gui_mkrc(paste_rc.x - 54, btn_y, 50, btn_h);

        if (gui_button_ex(g_win, copy_rc, "Copy", &g_ui, 1, sel_active)) {
            copy_to_parcel();
            if (sel_active) {
                sel_active = 0;
                dirty = 1;
            }
        }
        if (gui_button_ex(g_win, paste_rc, "Paste", &g_ui, shell_in >= 0, 0))
            paste_from_parcel();
        left_limit = copy_rc.x - 8;
    }

    if (status_msg[0]) {
        snprintf(left, sizeof(left), "%s", status_msg);
    } else if (fg_pid >= 0) {
        snprintf(left, sizeof(left), "running pid %d (ctrl-C interrupts)",
                 fg_pid);
    } else {
        snprintf(left, sizeof(left), "%s (%dx%d)", cwd, term_cols, term_rows);
    }

    gui_clip(g_win, gui_mkrc(0, r.y, left_limit, r.h));
    gui_text(g_win, 8, r.y + (STATUS_H - GUI_FONT_H) / 2, left,
             fg_pid >= 0 ? GUI_WARN : GUI_TEXT_DIM, GUI_TRANSPARENT);
    gui_unclip(g_win);

    gui_text(g_win, win_w - 8 - right_w,
             r.y + (STATUS_H - GUI_FONT_H) / 2, right,
             view > 0 ? GUI_ACCENT : GUI_TEXT_DIM, GUI_TRANSPARENT);
}

static void draw_grid(void)
{
    int r, c;

    for (r = 0; r < term_rows; r++) {
        int abs_r = base - view + r;
        const struct cell *row = row_at(abs_r);
        int y = PAD + r * GUI_FONT_H;

        for (c = 0; c < term_cols; c++) {
            int x = PAD + c * GUI_FONT_W;
            uint32_t bg = palette[row[c].bg & 15];
            uint32_t fg = palette[row[c].fg & 15];

            if (cell_is_selected(abs_r, c)) {
                bg = GUI_ACCENT_DARK;
                fg = GUI_WHITE;
            }
            if (row[c].ch == ' ' || row[c].ch == 0)
                gui_rect(g_win, x, y, GUI_FONT_W, GUI_FONT_H, bg);
            else
                gui_char(g_win, x, y, (char)row[c].ch, fg, (long)bg);
        }
    }

    if (cursor_on && view == 0) {
        int curx = cx < term_cols ? cx : term_cols - 1;
        int cury = cy < term_rows ? cy : term_rows - 1;
        int x = PAD + curx * GUI_FONT_W;
        int y = PAD + cury * GUI_FONT_H;
        const struct cell *row = screen_row(cury, 0);

        if (focused) {
            gui_rect(g_win, x, y, GUI_FONT_W, GUI_FONT_H, palette[7]);
            if (row[curx].ch > 32)
                gui_char(g_win, x, y, (char)row[curx].ch, palette[0],
                         GUI_TRANSPARENT);
        } else {
            gui_frame(g_win, x, y, GUI_FONT_W, GUI_FONT_H, palette[8]);
        }
    }
}

static void repaint(void)
{
    int grid_w = term_cols * GUI_FONT_W;
    int grid_h = term_rows * GUI_FONT_H;
    int top = back_count - view;

    dirty = 0;
    gui_rect(g_win, 0, 0, win_w, win_h - STATUS_H, palette[0]);
    draw_grid();
    gui_frame(g_win, PAD - 2, PAD - 2, grid_w + 4, grid_h + 4, GUI_BORDER);

    if (gui_scrollbar(g_win, gui_mkrc(win_w - PAD - BAR_W, PAD, BAR_W, grid_h),
                      back_count + term_rows, term_rows, &top, &g_ui)) {
        view = back_count - top;
        if (view < 0)
            view = 0;
        if (view > back_count)
            view = back_count;
    }
    draw_status();
    gui_flush(g_win);
}

/* ----------------------------------------------------- process helpers */

static int child_alive(int pid)
{
    struct k_psinfo ps;
    int i = 0;

    if (pid <= 0)
        return 0;
    while (psinfo(i++, &ps) == 0) {
        if (ps.pid == pid)
            return ps.state != K_STATE_ZOMBIE;
    }
    return 0;
}

/* Collect all live descendant PIDs of root_pid (excluding root_pid). */
static int find_descendants(int root_pid, int *out, int max_out)
{
    struct k_psinfo list[64];
    int nproc = 0, nout = 0;
    int i, added;

    if (root_pid <= 0)
        return 0;
    while (nproc < 64 && psinfo(nproc, &list[nproc]) == 0)
        nproc++;

    do {
        added = 0;
        for (i = 0; i < nproc && nout < max_out; i++) {
            int pid = list[i].pid;
            int ppid = list[i].ppid;
            int seen = 0, parent_match = (ppid == root_pid), j;

            if (pid <= 1 || pid == root_pid || list[i].state == K_STATE_ZOMBIE)
                continue;
            for (j = 0; j < nout; j++) {
                if (out[j] == pid) {
                    seen = 1;
                    break;
                }
                if (out[j] == ppid)
                    parent_match = 1;
            }
            if (!seen && parent_match) {
                out[nout++] = pid;
                added = 1;
            }
        }
    } while (added && nout < max_out);

    return nout;
}

static void refresh_fg_pid(void)
{
    int desc[16];
    int n = find_descendants(shell_pid, desc, 16);
    int next = (n > 0) ? desc[n - 1] : -1;

    if (next != fg_pid) {
        fg_pid = next;
        dirty = 1;
    }
}

/* ---------------------------------------------------- pipe /bin/sh I/O */

static int backend_start(void)
{
    int to_sh[2], from_sh[2];
    struct k_spawn_caps caps;
    char *argv[3];

    if (pipe_(to_sh) != 0)
        return -1;
    if (pipe_(from_sh) != 0) {
        close(to_sh[0]);
        close(to_sh[1]);
        return -1;
    }

    memset(&caps, 0, sizeof(caps));
    caps.fds[0].child_fd = 0;
    caps.fds[0].parent_fd = to_sh[0];
    caps.fds[1].child_fd = 1;
    caps.fds[1].parent_fd = from_sh[1];
    caps.fds[2].child_fd = 2;
    caps.fds[2].parent_fd = from_sh[1];
    caps.n_fds = 3;
    caps.flags = K_SPAWN_CLOSE_OTHER;
    snprintf(caps.cwd, sizeof(caps.cwd), "%s", cwd);

    argv[0] = "/bin/sh";
    argv[1] = "-i";
    argv[2] = 0;

    shell_pid = spawn_caps("/bin/sh", argv, &caps);
    close(to_sh[0]);
    close(from_sh[1]);
    if (shell_pid < 0) {
        close(to_sh[1]);
        close(from_sh[0]);
        return -1;
    }
    shell_in = to_sh[1];
    shell_out = from_sh[0];
    return 0;
}

static void backend_stop(void)
{
    int desc[32];
    int n, i;

    if (shell_in >= 0) {
        close(shell_in);
        shell_in = -1;
    }
    if (shell_pid >= 0) {
        n = find_descendants(shell_pid, desc, 32);
        for (i = n - 1; i >= 0; i--)
            kill(desc[i], SIGTERM);
        kill(shell_pid, SIGTERM);
        for (i = 0; i < 10 && child_alive(shell_pid); i++)
            sleep_ms(10);
        if (child_alive(shell_pid)) {
            for (i = n - 1; i >= 0; i--)
                kill(desc[i], SIGKILL);
            kill(shell_pid, SIGKILL);
        }
        waitpid(shell_pid);
        shell_pid = -1;
    }
    if (shell_out >= 0) {
        close(shell_out);
        shell_out = -1;
    }
}

/* Drain shell output non-blockingly so UI remains responsive. */
static int backend_poll(void)
{
    char buf[512];
    long n;
    int guard = 64;
    int got_any = 0;
    unsigned long now = uptime_ms();

    while (guard-- > 0 && shell_out >= 0) {
        n = read_nb(shell_out, buf, sizeof(buf));
        if (n <= 0)
            break;
        feed(buf, n);
        got_any = 1;
    }

    if (status_msg[0] && now >= status_until) {
        status_msg[0] = '\0';
        dirty = 1;
    }

    if (!got_any && now - last_proc_poll >= 100) {
        last_proc_poll = now;
        if (shell_pid >= 0 && !child_alive(shell_pid)) {
            while (shell_out >= 0) {
                n = read_nb(shell_out, buf, sizeof(buf));
                if (n <= 0)
                    break;
                feed(buf, n);
            }
            waitpid(shell_pid);
            shell_pid = -1;
            if (shell_in >= 0) {
                close(shell_in);
                shell_in = -1;
            }
            if (shell_out >= 0) {
                close(shell_out);
                shell_out = -1;
            }
            fg_pid = -1;
            want_exit = 1;
            return got_any;
        }
        refresh_fg_pid();
    }
    return got_any;
}

static void send_interrupt(void)
{
    int desc[32];
    int n, i;

    n = find_descendants(shell_pid, desc, 32);
    if (n > 0) {
        for (i = n - 1; i >= 0; i--)
            kill(desc[i], SIGINT);
        fg_pid = desc[n - 1];
        set_toast("^C (SIGINT)");
        return;
    }
    if (shell_in >= 0) {
        unsigned char b = 3;
        write(shell_in, &b, 1);
    }
}

/* -------------------------------------------------------------- events */

static void scroll_view(int delta)
{
    int old = view;

    view += delta;
    if (view < 0)
        view = 0;
    if (view > back_count)
        view = back_count;
    if (view != old)
        dirty = 1;
}

static void on_key(int k)
{
    if (k == KEY_PGUP) {
        scroll_view(term_rows / 2);
        return;
    }
    if (k == KEY_PGDN) {
        scroll_view(-(term_rows / 2));
        return;
    }
    if (k == KEY_WM_NEXT || k == KEY_WM_CLOSE || k == KEY_LAUNCHER ||
        (k >= KEY_WM_WS1 && k <= KEY_WM_WS4))
        return;

    if (k == 22) {              /* Ctrl+V / Ctrl+Shift+V: paste from Talon */
        paste_from_parcel();
        return;
    }
    if (k == 3) {               /* Ctrl+C / Ctrl+Shift+C */
        if (sel_active) {
            copy_to_parcel();
            sel_active = 0;
            dirty = 1;
            return;
        }
        send_interrupt();
        return;
    }

    if (view != 0) {
        view = 0;
        dirty = 1;
    }
    if (shell_in >= 0) {
        unsigned char b = (unsigned char)k;
        write(shell_in, &b, 1);
    }
}

static int mouse_to_cell(int mx, int my, int *abs_row, int *col)
{
    int grid_w = term_cols * GUI_FONT_W;
    int grid_h = term_rows * GUI_FONT_H;
    int sr, sc;

    if (mx < PAD || mx >= PAD + grid_w || my < PAD || my >= PAD + grid_h)
        return 0;
    sc = (mx - PAD) / GUI_FONT_W;
    sr = (my - PAD) / GUI_FONT_H;
    *abs_row = base - view + sr;
    *col = sc;
    return 1;
}

static void handle_event(const struct k_event *ev)
{
    gui_ui_event(&g_ui, ev);
    switch (ev->type) {
    case KEV_CLOSE:
        g_win->open = 0;
        break;
    case KEV_KEY:
        on_key((int)ev->key);
        break;
    case KEV_FOCUS:
        focused = ev->key != 0;
        dirty = 1;
        break;
    case KEV_MOUSE_WHEEL:
        scroll_view((int)(int32_t)ev->key * 3);
        break;
    case KEV_RESIZE:
        if (ev->x > 0 && ev->y > 0) {
            g_win->w = ev->x;
            g_win->h = ev->y;
            g_win->clip = gui_mkrc(0, 0, ev->x, ev->y);
            handle_resize(ev->x, ev->y);
        }
        break;
    case KEV_THEME:
        gui_settings_reload();
        dirty = 1;
        break;
    case KEV_MOUSE_DOWN: {
        int r, c;
        if ((ev->buttons == 0 || (ev->buttons & K_MOUSE_LEFT)) &&
            mouse_to_cell(ev->x, ev->y, &r, &c)) {
            sel_dragging = 1;
            sel_active = 0;
            sel_a_row = sel_b_row = r;
            sel_a_col = sel_b_col = c;
        } else {
            sel_dragging = 0;
            if (ev->y < win_h - STATUS_H && sel_active)
                sel_active = 0;
        }
        dirty = 1;
        break;
    }
    case KEV_MOUSE_MOVE: {
        int r, c;
        if (sel_dragging && mouse_to_cell(ev->x, ev->y, &r, &c)) {
            sel_b_row = r;
            sel_b_col = c;
            sel_active = (sel_b_row != sel_a_row || sel_b_col != sel_a_col);
        }
        dirty = 1;
        break;
    }
    case KEV_MOUSE_UP:
        sel_dragging = 0;
        dirty = 1;
        break;
    default:
        dirty = 1;
        break;
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct k_event ev;
    int i;

    (void)argc;
    (void)argv;
    snprintf(cwd, sizeof(cwd), "%s", kestrel_startup_cwd());

    for (i = 0; i < TOTAL; i++)
        clear_cells(grid[i], 0, MAX_COLS);

    g_win = gui_open("Terminal", 90, 60, DEF_WIN_W, DEF_WIN_H, 0);
    if (!g_win) {
        printf("terminal: no window manager available\n");
        return 1;
    }
    win_w = g_win->w;
    win_h = g_win->h;
    compute_grid_size(win_w, win_h, &term_cols, &term_rows);

    feed_str("\033[36mKestrelOS terminal\033[0m - /bin/sh\n");

    if (backend_start() != 0) {
        printf("terminal: cannot start /bin/sh\n");
        gui_close(g_win);
        return 1;
    }

    repaint();

    while (g_win->open && !want_exit) {
        int had_ev = 0;
        int got_out;

        gui_ui_begin(&g_ui);
        for (;;) {
            int got = gui_next_event(g_win, &ev, 0);
            if (got < 0)
                goto done;
            if (got == 0)
                break;
            had_ev = 1;
            handle_event(&ev);
            if (!g_win->open || want_exit)
                goto done;
        }

        got_out = backend_poll();
        if (want_exit)
            goto done;

        if (dirty && g_win->open)
            repaint();

        if (!had_ev && !got_out) {
            int got = gui_next_event(g_win, &ev, 16);
            if (got < 0)
                goto done;
            if (got > 0) {
                gui_ui_begin(&g_ui);
                handle_event(&ev);
                if (dirty && g_win->open)
                    repaint();
            }
        }
    }

done:
    backend_stop();
    gui_close(g_win);
    return 0;
}
