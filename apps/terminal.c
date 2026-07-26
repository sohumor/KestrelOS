/* terminal.c - a graphical terminal emulator for KestrelOS.
 *
 * An 80x24 character grid in a libgui window: a full ANSI-subset screen
 * emulator (CSI cursor motion, erase, SGR colours), 200 lines of
 * scrollback with keyboard and scrollbar navigation, a block cursor and
 * a status line.
 *
 * How the child process is wired up - read this before changing it
 * ---------------------------------------------------------------
 * The intended design is: SYS_PIPE twice, then spawn /bin/sh with its
 * stdin and stdout on the pipe ends, and pump bytes both ways. That is
 * NOT possible on the current ABI: SYS_SPAWN_IO (abi/kestrel_abi.h,
 * struct k_spawn_io) redirects a child by *path* only, and a pipe has no
 * path - kernel/include/pipe.h says so plainly, pipes are anonymous, and
 * /dev has no fd namespace either. There is no syscall that hands a
 * child an existing descriptor.
 *
 * So this program ships two back ends:
 *
 *   TERM_USE_PIPES 0 (default, works today)
 *       The terminal is its own small shell: it does the line editing,
 *       the history and the builtins (cd/pwd/clear/exit/help), and runs
 *       external commands with spawn_io(cmd, argv, "/dev/null", tmp, 0),
 *       streaming the temp file into the grid while the child runs so
 *       long-running output appears as it is produced and the UI never
 *       blocks. It is not the real shell and it says so on the status
 *       line. Redirection, pipes and $-expansion are the real shell's
 *       job and are not reimplemented here.
 *
 *   TERM_USE_PIPES 1 (one kernel syscall away)
 *       The real thing: two pipes, /bin/sh spawned onto them, keystrokes
 *       written to its stdin, its output read back non-blocking and fed
 *       to the emulator. Compiles today; needs the kernel to grow a
 *       spawn variant that takes descriptors (see the integration notes
 *       and SYS_SPAWN_FD below).
 *
 * Either way the emulator, the renderer and the event loop are the same
 * code, so the switch is one #define once the syscall exists.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#ifndef TERM_USE_PIPES
#define TERM_USE_PIPES 0
#endif

/* ------------------------------------------------------------ geometry */

#define COLS        80
#define ROWS        24
#define SCROLLBACK  200
#define TOTAL       (ROWS + SCROLLBACK)

#define PAD         6
#define BAR_W       14
#define STATUS_H    22
#define GRID_W      (COLS * GUI_FONT_W)
#define GRID_H      (ROWS * GUI_FONT_H)
#define WIN_W       (PAD + GRID_W + 4 + BAR_W + PAD)
#define WIN_H       (PAD + GRID_H + PAD + STATUS_H)

#define MAX_LINE    512
#define MAX_TOKENS  16
#define MAX_PATH    256
#define HIST_MAX    32

/* ANSI colours, in the same slate family as the desktop theme so a
 * terminal window does not look like it came from another machine. */
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

static struct cell grid[TOTAL][COLS];
static int base;                /* ring index of screen row 0        */
static int back_count;          /* scrollback rows currently stored  */
static int view;                /* rows scrolled back, 0 = live      */
static int cx, cy;              /* cursor, screen coordinates        */
static int cur_fg = DEF_FG, cur_bg = DEF_BG;
static int bold, reverse;
static int cursor_on = 1;
static int saved_cx, saved_cy;
static int dirty = 1;
static int focused = 1;

static int esc_state;           /* 0 text, 1 ESC, 2 CSI, 3/4 OSC     */
static char csi[32];
static int csi_len;

static gui_window *g_win;
static gui_ui g_ui;
static char cwd[MAX_PATH] = "/";
static int running_pid = -1;
static int want_exit;
static char status_msg[80];

/* --------------------------------------------------------- grid helpers */

static struct cell *row_at(int ring)
{
    return grid[((ring % TOTAL) + TOTAL) % TOTAL];
}

/* Ring index of screen row `r`, honouring the scrollback offset. */
static struct cell *screen_row(int r, int offset)
{
    return row_at(base - offset + r);
}

static void clear_cells(struct cell *row, int from, int to)
{
    int i;

    for (i = from; i < to && i < COLS; i++) {
        row[i].ch = ' ';
        row[i].fg = (unsigned char)DEF_FG;
        row[i].bg = (unsigned char)cur_bg;
    }
}

static int input_row = -1;      /* screen row the prompt starts on */

static void scroll_up_one(void)
{
    base++;
    if (back_count < TOTAL - ROWS)
        back_count++;
    clear_cells(screen_row(ROWS - 1, 0), 0, COLS);
    if (input_row >= 0)
        input_row--;
    if (view > 0 && view < back_count)
        view++;                 /* keep a scrolled-back view steady */
    dirty = 1;
}

static void line_feed(void)
{
    cy++;
    if (cy >= ROWS) {
        cy = ROWS - 1;
        scroll_up_one();
    }
}

static void put_glyph(unsigned char c)
{
    struct cell *row;
    int fg = bold ? (cur_fg | 8) : cur_fg;
    int bg = cur_bg;

    if (cx >= COLS) {
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
        for (r = 0; r < ROWS; r++)
            clear_cells(screen_row(r, 0), 0, COLS);
    } else if (mode == 1) {
        for (r = 0; r < cy; r++)
            clear_cells(screen_row(r, 0), 0, COLS);
        clear_cells(screen_row(cy, 0), 0, cx + 1);
    } else {
        clear_cells(screen_row(cy, 0), cx, COLS);
        for (r = cy + 1; r < ROWS; r++)
            clear_cells(screen_row(r, 0), 0, COLS);
    }
    dirty = 1;
}

static void erase_line(int mode)
{
    struct cell *row = screen_row(cy, 0);

    if (mode == 1)
        clear_cells(row, 0, cx + 1);
    else if (mode == 2)
        clear_cells(row, 0, COLS);
    else
        clear_cells(row, cx, COLS);
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
    if (cx > COLS - 1)
        cx = COLS - 1;
    if (cy < 0)
        cy = 0;
    if (cy > ROWS - 1)
        cy = ROWS - 1;
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
        break;                  /* unknown finals are dropped, not drawn */
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
        view = 0;               /* new output snaps the view back live */
}

static void feed_str(const char *s)
{
    feed(s, (long)strlen(s));
}

#if !TERM_USE_PIPES
static void feedf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void feedf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    feed_str(buf);
}
#endif

/* ------------------------------------------------------------ rendering */

static void draw_status(void)
{
    gui_rc r = gui_mkrc(0, WIN_H - STATUS_H, WIN_W, STATUS_H);
    char left[96];
    const char *right;

    gui_rect(g_win, r.x, r.y, r.w, r.h, GUI_PANEL);
    gui_rect(g_win, r.x, r.y, r.w, 1, GUI_BORDER);

    if (status_msg[0])
        snprintf(left, sizeof(left), "%s", status_msg);
    else if (running_pid >= 0)
        snprintf(left, sizeof(left), "running pid %d  (ctrl-C stops it)",
                 running_pid);
    else
        snprintf(left, sizeof(left), "%s", cwd);
    gui_text(g_win, 8, r.y + (STATUS_H - GUI_FONT_H) / 2, left,
             running_pid >= 0 ? GUI_WARN : GUI_TEXT_DIM, GUI_TRANSPARENT);

    if (view > 0) {
        char sb[48];

        snprintf(sb, sizeof(sb), "scrollback -%d/%d", view, back_count);
        gui_text(g_win, WIN_W - 8 - gui_text_w(sb),
                 r.y + (STATUS_H - GUI_FONT_H) / 2, sb, GUI_ACCENT,
                 GUI_TRANSPARENT);
        return;
    }
    right = TERM_USE_PIPES ? "/bin/sh" : "built-in shell";
    gui_text(g_win, WIN_W - 8 - gui_text_w(right),
             r.y + (STATUS_H - GUI_FONT_H) / 2, right, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
}

static void draw_grid(void)
{
    int r, c;

    for (r = 0; r < ROWS; r++) {
        const struct cell *row = screen_row(r, view);
        int y = PAD + r * GUI_FONT_H;

        for (c = 0; c < COLS; c++) {
            int x = PAD + c * GUI_FONT_W;
            uint32_t bg = palette[row[c].bg & 15];
            uint32_t fg = palette[row[c].fg & 15];

            if (row[c].ch == ' ' || row[c].ch == 0)
                gui_rect(g_win, x, y, GUI_FONT_W, GUI_FONT_H, bg);
            else
                gui_char(g_win, x, y, (char)row[c].ch, fg, (long)bg);
        }
    }

    if (cursor_on && view == 0) {
        int x = PAD + cx * GUI_FONT_W;
        int y = PAD + cy * GUI_FONT_H;
        const struct cell *row = screen_row(cy, 0);

        if (focused) {
            gui_rect(g_win, x, y, GUI_FONT_W, GUI_FONT_H, palette[7]);
            if (row[cx].ch > 32)
                gui_char(g_win, x, y, (char)row[cx].ch, palette[0],
                         GUI_TRANSPARENT);
        } else {
            gui_frame(g_win, x, y, GUI_FONT_W, GUI_FONT_H, palette[8]);
        }
    }
}

static void repaint(void)
{
    int top = back_count - view;

    gui_rect(g_win, 0, 0, WIN_W, WIN_H - STATUS_H, palette[0]);
    draw_grid();
    gui_frame(g_win, PAD - 2, PAD - 2, GRID_W + 4, GRID_H + 4, GUI_BORDER);

    if (gui_scrollbar(g_win, gui_mkrc(PAD + GRID_W + 4, PAD, BAR_W, GRID_H),
                      back_count + ROWS, ROWS, &top, &g_ui)) {
        view = back_count - top;
        if (view < 0)
            view = 0;
        if (view > back_count)
            view = back_count;
    }
    draw_status();
    gui_flush(g_win);
    dirty = 0;
}

/* ------------------------------------------------- path helpers
 * Only the built-in shell resolves paths; with the real /bin/sh behind
 * a pipe that is the shell's job, so they compile out. */
#if !TERM_USE_PIPES

static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", cwd, tok);
}

/* Collapse "." and ".." in place; the result always starts with '/'. */
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
                len--;                  /* drop the separator too */
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

#endif /* !TERM_USE_PIPES */

/* ------------------------------------------------------------ back ends */

#if TERM_USE_PIPES

/* The descriptor-passing spawn this back end needs. The number must match
 * whatever the kernel assigns; it is defined here only so the code
 * compiles before the syscall exists. */
#ifndef SYS_SPAWN_FD
#define SYS_SPAWN_FD 53
#endif

struct term_spawn_fd {
    int32_t in_fd;
    int32_t out_fd;
    int32_t err_fd;             /* -1 keeps the console */
    int32_t _pad;
};

static int shell_in = -1;       /* write end: keystrokes to the shell */
static int shell_out = -1;      /* read end: shell output             */
static int shell_pid = -1;

static int backend_start(void)
{
    int to_sh[2], from_sh[2];
    struct term_spawn_fd io;
    char *argv[2];

    if (syscall(SYS_PIPE, (long)to_sh, 0, 0, 0) != 0)
        return -1;
    if (syscall(SYS_PIPE, (long)from_sh, 0, 0, 0) != 0) {
        close(to_sh[0]);
        close(to_sh[1]);
        return -1;
    }
    io.in_fd = to_sh[0];
    io.out_fd = from_sh[1];
    io.err_fd = from_sh[1];
    io._pad = 0;
    argv[0] = "/bin/sh";
    argv[1] = 0;
    shell_pid = (int)syscall(SYS_SPAWN_FD, (long)"/bin/sh", (long)argv,
                             (long)&io, 0);
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
    if (shell_pid >= 0)
        gui_kill(shell_pid);
    if (shell_in >= 0)
        close(shell_in);
    if (shell_out >= 0)
        close(shell_out);
    shell_pid = shell_in = shell_out = -1;
}

/* Drain whatever the shell has produced. Non-blocking, so the UI keeps
 * running while a command is still working. */
static void backend_poll(void)
{
    char buf[512];
    long n;
    int guard = 32;

    while (guard-- > 0 && shell_out >= 0) {
        n = read_nb(shell_out, buf, sizeof(buf));
        if (n <= 0)
            break;
        feed(buf, n);
    }
}

static void backend_key(int k)
{
    unsigned char b = (unsigned char)k;

    if (shell_in >= 0)
        write(shell_in, &b, 1);
}

#else  /* ---------------- built-in shell over spawn_io ---------------- */

static char tmp_out[64];
static char line[MAX_LINE];
static int line_len, line_caret;
static char hist[HIST_MAX][MAX_LINE];
static int hist_count, hist_pos;
static int last_status;

static void prompt(void);

/* Pick a writable place for the capture file: /tmp if it exists or can be
 * created, otherwise the root directory. Failure is reported once. */
static void pick_tmp(void)
{
    static const char *dirs[3] = { "/tmp", "/var/tmp", "" };
    int i, fd;

    mkdir_("/tmp");
    for (i = 0; i < 3; i++) {
        snprintf(tmp_out, sizeof(tmp_out), "%s/.term-%d.out", dirs[i],
                 getpid());
        fd = open(tmp_out, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            close(fd);
            unlink_(tmp_out);
            return;
        }
    }
    tmp_out[0] = '\0';
}

static int backend_start(void)
{
    pick_tmp();
    if (!tmp_out[0])
        snprintf(status_msg, sizeof(status_msg),
                 "no writable directory: commands cannot be run");
    return 0;
}

static void backend_stop(void)
{
    if (tmp_out[0])
        unlink_(tmp_out);
}

static void backend_poll(void)
{
}

/* --- the line editor ------------------------------------------------- */

static int prompt_len;

static void erase_to_end(void)
{
    int r;

    clear_cells(screen_row(cy, 0), cx, COLS);
    for (r = cy + 1; r < ROWS; r++)
        clear_cells(screen_row(r, 0), 0, COLS);
}

static void redraw_input(void)
{
    int pos;

    if (input_row < 0)
        input_row = 0;
    cy = input_row;
    cx = 0;
    erase_to_end();
    feedf("kestrel:%s$ ", cwd);
    prompt_len = 8 + (int)strlen(cwd) + 2;
    feed(line, line_len);
    pos = prompt_len + line_caret;
    cy = input_row + pos / COLS;
    cx = pos % COLS;
    clamp_cursor();
    dirty = 1;
}

static void prompt(void)
{
    if (cx != 0)
        feed_str("\r\n");
    input_row = cy;
    line_len = 0;
    line_caret = 0;
    line[0] = '\0';
    hist_pos = hist_count;
    redraw_input();
}

static void hist_add(const char *s)
{
    int i;

    if (!s[0])
        return;
    if (hist_count > 0 && strcmp(hist[hist_count - 1], s) == 0)
        return;
    if (hist_count == HIST_MAX) {
        for (i = 1; i < HIST_MAX; i++)
            strcpy(hist[i - 1], hist[i]);
        hist_count--;
    }
    strncpy(hist[hist_count], s, MAX_LINE - 1);
    hist[hist_count][MAX_LINE - 1] = '\0';
    hist_count++;
}

static void hist_recall(int delta)
{
    int p = hist_pos + delta;

    if (p < 0 || p > hist_count)
        return;
    hist_pos = p;
    if (p == hist_count) {
        line_len = 0;
        line[0] = '\0';
    } else {
        strncpy(line, hist[p], MAX_LINE - 1);
        line[MAX_LINE - 1] = '\0';
        line_len = (int)strlen(line);
    }
    line_caret = line_len;
    redraw_input();
}

/* --- running commands ------------------------------------------------ */

static void pump_events(void);

static int child_alive(int pid)
{
    struct k_psinfo ps;
    int i = 0;

    while (psinfo(i++, &ps) == 0) {
        if (ps.pid == pid)
            return ps.state != K_STATE_ZOMBIE;
    }
    return 0;
}

/* Stream the capture file into the grid while the child runs. Returns
 * when the child is gone and the file has been drained. */
static void stream_child(int pid)
{
    char buf[512];
    int fd = -1, tries = 0;
    long n;

    running_pid = pid;
    while (fd < 0 && tries++ < 20) {
        fd = open(tmp_out, O_RDONLY);
        if (fd < 0)
            sleep_ms(10);
    }

    for (;;) {
        int alive = child_alive(pid);

        if (fd >= 0) {
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                feed(buf, n);
        }
        if (!alive)
            break;
        pump_events();
        if (!g_win->open) {
            /* The window went away: do not leave a child writing into a
             * capture file nobody will ever read. */
            gui_kill(pid);
            break;
        }
        if (dirty)
            repaint();
        sleep_ms(15);
    }
    if (fd >= 0)
        close(fd);
    running_pid = -1;
}

static int tokenize(char *buf, char **argv, int max)
{
    int n = 0;

    while (*buf && n < max) {
        while (*buf == ' ' || *buf == '\t')
            buf++;
        if (!*buf)
            break;
        if (*buf == '"') {
            buf++;
            argv[n++] = buf;
            while (*buf && *buf != '"')
                buf++;
        } else {
            argv[n++] = buf;
            while (*buf && *buf != ' ' && *buf != '\t')
                buf++;
        }
        if (*buf)
            *buf++ = '\0';
    }
    return n;
}

static void cmd_cd(const char *arg)
{
    char path[MAX_PATH];
    struct k_stat st;

    if (!arg || !arg[0])
        arg = "/";
    resolve(arg, path, sizeof(path));
    normalize(path);
    if (stat_(path, &st) != 0 || !st.is_dir) {
        feedf("terminal: no such directory: %s\r\n", path);
        last_status = 1;
        return;
    }
    strncpy(cwd, path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
    last_status = 0;
}

static void cmd_help(void)
{
    feed_str("\033[1;36mKestrelOS terminal\033[0m\r\n");
    feed_str("  builtins: cd, pwd, clear, history, help, exit\r\n");
    feed_str("  anything else runs /bin/<name> with its output captured\r\n");
    feed_str("  keys: PgUp/PgDn scroll back, ctrl-C stops a command,\r\n");
    feed_str("        up/down recall history, ctrl-U clears the line\r\n");
    feed_str("  redirection and pipes belong to /bin/sh, not to this\r\n");
    feed_str("  built-in shell - run \033[36msh\033[0m on the text console"
             " for those\r\n");
}

static void run_line(char *text)
{
    char work[MAX_LINE];
    char *tok[MAX_TOKENS + 1];
    char cwdarg[MAX_PATH + 8];
    char path[MAX_PATH];
    struct k_stat st;
    int n, i, pid;

    strncpy(work, text, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    n = tokenize(work, tok, MAX_TOKENS);
    if (n == 0)
        return;

    if (strcmp(tok[0], "exit") == 0) {
        want_exit = 1;
        return;
    }
    if (strcmp(tok[0], "cd") == 0) {
        cmd_cd(n > 1 ? tok[1] : 0);
        return;
    }
    if (strcmp(tok[0], "pwd") == 0) {
        feedf("%s\r\n", cwd);
        return;
    }
    if (strcmp(tok[0], "clear") == 0) {
        erase_screen(2);
        cx = 0;
        cy = 0;
        return;
    }
    if (strcmp(tok[0], "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(tok[0], "history") == 0) {
        for (i = 0; i < hist_count; i++)
            feedf("%4d  %s\r\n", i + 1, hist[i]);
        return;
    }

    if (strchr(tok[0], '/'))
        resolve(tok[0], path, sizeof(path));
    else
        snprintf(path, sizeof(path), "/bin/%s", tok[0]);
    if (stat_(path, &st) != 0 || st.is_dir) {
        feedf("terminal: command not found: %s\r\n", tok[0]);
        last_status = 127;
        return;
    }
    if (!tmp_out[0]) {
        feed_str("terminal: no writable directory for command output\r\n");
        last_status = 1;
        return;
    }

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", cwd);
    if (n < MAX_TOKENS)
        tok[n++] = cwdarg;
    tok[n] = 0;

    /* stdin comes from /dev/null: a program that waits on the console
     * would otherwise steal the keyboard from behind the desktop. */
    pid = spawn_io(path, tok, "/dev/null", tmp_out, 0);
    if (pid < 0) {
        feedf("terminal: cannot run %s\r\n", path);
        last_status = 1;
        return;
    }
    stream_child(pid);
    last_status = waitpid(pid);
    unlink_(tmp_out);
    if (last_status != 0)
        feedf("\033[33m[exit %d]\033[0m\r\n", last_status);
}

static void backend_key(int k)
{
    int i;

    if (running_pid >= 0) {
        if (k == 3)
            gui_kill(running_pid);
        return;
    }

    switch (k) {
    case '\n':
    case '\r':
        cy = input_row + (prompt_len + line_len) / COLS;
        cx = (prompt_len + line_len) % COLS;
        clamp_cursor();
        feed_str("\r\n");
        line[line_len] = '\0';
        hist_add(line);
        run_line(line);
        prompt();
        return;
    case 8:
    case 127:
        if (line_caret > 0) {
            memmove(line + line_caret - 1, line + line_caret,
                    (unsigned long)(line_len - line_caret));
            line_len--;
            line_caret--;
            line[line_len] = '\0';
        }
        break;
    case KEY_DELETE:
        if (line_caret < line_len) {
            memmove(line + line_caret, line + line_caret + 1,
                    (unsigned long)(line_len - line_caret - 1));
            line_len--;
            line[line_len] = '\0';
        }
        break;
    case KEY_LEFT:
        if (line_caret > 0)
            line_caret--;
        break;
    case KEY_RIGHT:
        if (line_caret < line_len)
            line_caret++;
        break;
    case KEY_HOME:
    case 1:
        line_caret = 0;
        break;
    case KEY_END:
    case 5:
        line_caret = line_len;
        break;
    case KEY_UP:
        hist_recall(-1);
        return;
    case KEY_DOWN:
        hist_recall(1);
        return;
    case 3:                     /* ctrl-C: abandon the line */
        feed_str("^C\r\n");
        prompt();
        return;
    case 21:                    /* ctrl-U */
        line_len = 0;
        line_caret = 0;
        line[0] = '\0';
        break;
    case 11:                    /* ctrl-K: kill to end of line */
        line_len = line_caret;
        line[line_len] = '\0';
        break;
    case 12:                    /* ctrl-L: clear the screen */
        erase_screen(2);
        cx = 0;
        cy = 0;
        input_row = 0;
        redraw_input();
        return;
    default:
        if (k >= 32 && k < 127 && line_len < MAX_LINE - 1) {
            for (i = line_len; i > line_caret; i--)
                line[i] = line[i - 1];
            line[line_caret++] = (char)k;
            line_len++;
            line[line_len] = '\0';
        } else {
            return;
        }
        break;
    }
    redraw_input();
}

#endif /* TERM_USE_PIPES */

/* -------------------------------------------------------------- events */

static void on_key(int k)
{
    switch (k) {
    case KEY_PGUP:
        view += ROWS / 2;
        if (view > back_count)
            view = back_count;
        dirty = 1;
        return;
    case KEY_PGDN:
        view -= ROWS / 2;
        if (view < 0)
            view = 0;
        dirty = 1;
        return;
    default:
        break;
    }
    if (view != 0) {
        view = 0;               /* typing returns to the live screen */
        dirty = 1;
    }
    backend_key(k);
}

#if !TERM_USE_PIPES
/* Drain the event queue without blocking: used while a child command is
 * running so the window still scrolls, repaints and closes. */
static void pump_events(void)
{
    struct k_event ev;

    gui_ui_begin(&g_ui);
    for (;;) {
        int got = gui_next_event(g_win, &ev, 0);

        if (got <= 0)
            break;
        gui_ui_event(&g_ui, &ev);
        switch (ev.type) {
        case KEV_CLOSE:
            return;
        case KEV_KEY:
            on_key((int)ev.key);
            break;
        case KEV_FOCUS:
            focused = ev.key != 0;
            dirty = 1;
            break;
        case KEV_MOUSE_DOWN:
        case KEV_MOUSE_UP:
        case KEV_MOUSE_MOVE:
            dirty = 1;          /* the scrollbar may need to react */
            break;
        default:
            break;
        }
    }
}
#endif /* !TERM_USE_PIPES */

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct k_event ev;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            strncpy(cwd, argv[i] + 6, sizeof(cwd) - 1);
            cwd[sizeof(cwd) - 1] = '\0';
        }
    }

    for (i = 0; i < TOTAL; i++)
        clear_cells(grid[i], 0, COLS);

    g_win = gui_open("Terminal", 90, 60, WIN_W, WIN_H, 0);
    if (!g_win) {
        printf("terminal: no window manager available\n");
        return 1;
    }

    if (backend_start() != 0) {
        printf("terminal: cannot start the shell\n");
        gui_close(g_win);
        return 1;
    }

    feed_str("\033[36mKestrelOS terminal\033[0m - type "
             "\033[1mhelp\033[0m for the built-in commands\r\n");
#if !TERM_USE_PIPES
    prompt();
#endif
    repaint();

    while (g_win->open && !want_exit) {
        int got = gui_next_event(g_win, &ev, 20);

        if (got < 0)
            break;
        if (got > 0) {
            gui_ui_begin(&g_ui);
            gui_ui_event(&g_ui, &ev);
            switch (ev.type) {
            case KEV_CLOSE:
                g_win->open = 0;
                break;
            case KEV_KEY:
                on_key((int)ev.key);
                break;
            case KEV_FOCUS:
                focused = ev.key != 0;
                dirty = 1;
                break;
            default:
                dirty = 1;
                break;
            }
        } else {
            gui_ui_begin(&g_ui);
        }
        backend_poll();
        if (dirty && g_win->open)
            repaint();
    }

    backend_stop();
    gui_close(g_win);
    return 0;
}
