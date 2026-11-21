/* gpaint.c - KestrelOS graphical paint program.
 *
 * Tools: Pen, Eraser, Flood Fill, Line, Rect, Circle (outline or filled),
 * Eyedropper, 16-colour palette, 5 brush sizes, multi-step Undo history
 * ring (Ctrl+Z / u), KPX1 and 24/32-bit BMP compatibility, Talon
 * [Copy Hex] / [Pick Talon] parcel integration (K_PARCEL_COLOR), and
 * dynamic KEV_RESIZE handling.
 *
 * Right-click anywhere on the canvas samples the colour under the cursor
 * with any tool.  When the file box is not focused the keys p/e/f/l/r/c/i
 * switch tool, [ and ] shrink/grow the brush, and u (or Ctrl+Z) undoes.
 *
 * usage: gpaint [--cwd=<dir>] [file.kpx|file.bmp]
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define INIT_W     800
#define INIT_H     580
#define TOOL_W     118
#define TOP_H      40
#define STATUS_H   24
#define CANVAS_X   TOOL_W
#define CANVAS_Y   TOP_H
#define CANVAS_W   (INIT_W - TOOL_W)
#define CANVAS_H   (INIT_H - TOP_H - STATUS_H)
#define MAX_PATH   256

#define KPX_MAGIC  "KPX1"
#define KPX_HEADER 16
#define UNDO_MAX   10

enum paint_tool {
    TOOL_PEN = 0,
    TOOL_ERASER,
    TOOL_FILL,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_CIRCLE,
    TOOL_PICK,
    TOOL_COUNT
};

static const char *const tool_names[TOOL_COUNT] = {
    "Pen", "Eraser", "Fill", "Line", "Rect", "Circle", "Pick"
};

static const uint32_t swatches[16] = {
    0x000000U, 0xFFFFFFU, 0x808080U, 0xC8C8C8U,
    0xB03A2EU, 0xE67E22U, 0xE8C547U, 0x4F8F4FU,
    0x2E8B77U, 0x2E6DB4U, 0x3B4E9BU, 0x7D4E9BU,
    0xC2529BU, 0x8B5A2BU, 0xD9C7A0U, 0x1B2A3AU
};

static const int brushes[5] = { 1, 2, 4, 8, 16 };

static gui_window canvas;
static uint32_t canvas_bg = 0xFFFFFFU;
static uint32_t colour = 0x000000U;
static int brush = 2;
static int tool = TOOL_PEN;
static int shape_filled = 0;

static int last_x = -1, last_y = -1;
static int shape_drag = 0, shape_x0 = 0, shape_y0 = 0, shape_x1 = 0, shape_y1 = 0;

static char g_cwd[MAX_PATH];
static char status[128];
static char namebuf[MAX_PATH] = "/paint.kpx";
static gui_textbox_state namebox;

/* Multi-step undo snapshot ring */
static uint32_t *undo_ring[UNDO_MAX];
static int undo_head = 0;   /* next write slot */
static int undo_count = 0;  /* number of valid snapshots */

/* ------------------------------------------------------------- helpers */

static int lower_ch(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static int ends_with_ci(const char *s, const char *suf)
{
    unsigned long sl = strlen(s);
    unsigned long fl = strlen(suf);

    if (sl < fl)
        return 0;
    s += sl - fl;
    for (unsigned long i = 0; i < fl; i++) {
        if (lower_ch((unsigned char)s[i]) != lower_ch((unsigned char)suf[i]))
            return 0;
    }
    return 1;
}

static void resolve_path(const char *raw, char *out, unsigned long outsz)
{
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return;
    }
    if (raw[0] == '/')
        snprintf(out, outsz, "%s", raw);
    else if (strcmp(g_cwd, "/") == 0)
        snprintf(out, outsz, "/%s", raw);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, raw);
}

static uint32_t isqrt_u(unsigned long v)
{
    unsigned long x = v, y;

    if (v == 0)
        return 0;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (uint32_t)x;
}

/* ----------------------------------------------------------- undo ring */

static void undo_push(void)
{
    unsigned long bytes = (unsigned long)canvas.w * (unsigned long)canvas.h *
                          sizeof(uint32_t);
    uint32_t *slot = undo_ring[undo_head];

    if (!slot) {
        slot = malloc(bytes);
        if (!slot)
            return;
        undo_ring[undo_head] = slot;
    }
    memcpy(slot, canvas.px, bytes);
    undo_head = (undo_head + 1) % UNDO_MAX;
    if (undo_count < UNDO_MAX)
        undo_count++;
}

static int undo_pop(void)
{
    unsigned long bytes = (unsigned long)canvas.w * (unsigned long)canvas.h *
                          sizeof(uint32_t);

    if (undo_count <= 0)
        return -1;
    undo_head = (undo_head + UNDO_MAX - 1) % UNDO_MAX;
    undo_count--;
    if (undo_ring[undo_head])
        memcpy(canvas.px, undo_ring[undo_head], bytes);
    return 0;
}

static void undo_free_all(void)
{
    for (int i = 0; i < UNDO_MAX; i++) {
        free(undo_ring[i]);
        undo_ring[i] = 0;
    }
    undo_count = 0;
    undo_head = 0;
}

/* ------------------------------------------------------------ drawing */

static void canvas_dot(int x, int y, uint32_t c)
{
    int r = brushes[brush] / 2;

    if (r < 1)
        gui_pixel(&canvas, x, y, c);
    else
        gui_disc(&canvas, x, y, r, c);
}

static void canvas_stroke(int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps, i;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    steps = dx > dy ? dx : dy;
    if (steps == 0) {
        canvas_dot(x0, y0, c);
        return;
    }
    for (i = 0; i <= steps; i++)
        canvas_dot(x0 + ((x1 - x0) * i) / steps,
                   y0 + ((y1 - y0) * i) / steps, c);
}

static void canvas_clear(void)
{
    gui_rect(&canvas, 0, 0, canvas.w, canvas.h, canvas_bg);
}

static int canvas_flood_fill(int sx, int sy, uint32_t fill_col)
{
    uint32_t target;
    int *q;
    unsigned long head = 0, tail = 0, cap;

    if (sx < 0 || sy < 0 || sx >= canvas.w || sy >= canvas.h)
        return 0;
    fill_col &= 0x00FFFFFFU;
    target = canvas.px[(long)sy * canvas.w + sx] & 0x00FFFFFFU;
    if (target == fill_col)
        return 0;

    cap = (unsigned long)canvas.w * (unsigned long)canvas.h;
    q = malloc(cap * sizeof(int));
    if (!q)
        return 0;

    undo_push();
    canvas.px[(long)sy * canvas.w + sx] = fill_col;
    q[tail++] = sy * canvas.w + sx;

    while (head < tail) {
        int idx = q[head++];
        int x = idx % canvas.w;
        int y = idx / canvas.w;
        const int dx[4] = { 1, -1, 0, 0 };
        const int dy[4] = { 0, 0, 1, -1 };

        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k];
            int ny = y + dy[k];
            if (nx >= 0 && ny >= 0 && nx < canvas.w && ny < canvas.h) {
                long nidx = (long)ny * canvas.w + nx;
                if ((canvas.px[nidx] & 0x00FFFFFFU) == target) {
                    canvas.px[nidx] = fill_col;
                    if (tail < cap)
                        q[tail++] = (int)nidx;
                }
            }
        }
    }
    free(q);
    return 1;
}

/* Eyedropper: adopt the colour of the canvas pixel at (x, y). The caller
 * guarantees the point is inside the canvas. */
static void canvas_pick_colour(int x, int y)
{
    colour = canvas.px[(long)y * canvas.w + x] & 0x00FFFFFFU;
    snprintf(status, sizeof(status), "Picked #%02X%02X%02X at %d,%d",
             GUI_RED(colour), GUI_GREEN(colour), GUI_BLUE(colour), x, y);
}

static void draw_shape_to(gui_window *dst, int ox, int oy, int sh_tool,
                          int x0, int y0, int x1, int y1,
                          uint32_t c, int thick, int filled)
{
    int ax = ox + x0, ay = oy + y0;
    int bx = ox + x1, by = oy + y1;

    if (sh_tool == TOOL_LINE) {
        gui_line_w(dst, ax, ay, bx, by, c, thick);
    } else if (sh_tool == TOOL_RECT) {
        int rx = ax < bx ? ax : bx;
        int ry = ay < by ? ay : by;
        int rw = (ax > bx ? ax - bx : bx - ax) + 1;
        int rh = (ay > by ? ay - by : by - ay) + 1;

        if (filled) {
            gui_rect(dst, rx, ry, rw, rh, c);
        } else {
            for (int t = 0; t < thick && rw - 2 * t > 0 && rh - 2 * t > 0; t++)
                gui_frame(dst, rx + t, ry + t, rw - 2 * t, rh - 2 * t, c);
        }
    } else if (sh_tool == TOOL_CIRCLE) {
        long dx = (long)x1 - x0;
        long dy = (long)y1 - y0;
        int r = (int)isqrt_u((unsigned long)(dx * dx + dy * dy));

        if (filled) {
            gui_fill_circle(dst, ax, ay, r, c);
        } else {
            int half = thick / 2;
            for (int t = -half; t < thick - half; t++) {
                if (r + t >= 0)
                    gui_circle(dst, ax, ay, r + t, c);
            }
        }
    }
}

/* ------------------------------------------------------------ file I/O */

static void put_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint16_t get_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int write_all_fd(int fd, const void *buf, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long off = 0;

    while (off < len) {
        long w = write(fd, p + off, len - off);
        if (w <= 0)
            return -1;
        off += (unsigned long)w;
    }
    return 0;
}

static int save_bmp(const char *path)
{
    unsigned char hdr[54];
    unsigned long row_bytes = ((unsigned long)canvas.w * 3UL + 3UL) & ~3UL;
    unsigned long img_bytes = row_bytes * (unsigned long)canvas.h;
    unsigned char *rowbuf;
    int fd;

    rowbuf = malloc(row_bytes);
    if (!rowbuf)
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(rowbuf);
        return -1;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_le32(hdr + 2, (uint32_t)(54 + img_bytes));
    put_le32(hdr + 10, 54);
    put_le32(hdr + 14, 40);
    put_le32(hdr + 18, (uint32_t)canvas.w);
    put_le32(hdr + 22, (uint32_t)canvas.h);
    put_le16(hdr + 26, 1);
    put_le16(hdr + 28, 24);
    put_le32(hdr + 34, (uint32_t)img_bytes);

    if (write_all_fd(fd, hdr, sizeof(hdr)) != 0) {
        close(fd);
        free(rowbuf);
        return -1;
    }
    for (int y = canvas.h - 1; y >= 0; y--) {
        const uint32_t *src = canvas.px + (long)y * canvas.w;
        memset(rowbuf, 0, row_bytes);
        for (int x = 0; x < canvas.w; x++) {
            uint32_t px = src[x];
            rowbuf[x * 3 + 0] = (unsigned char)GUI_BLUE(px);
            rowbuf[x * 3 + 1] = (unsigned char)GUI_GREEN(px);
            rowbuf[x * 3 + 2] = (unsigned char)GUI_RED(px);
        }
        if (write_all_fd(fd, rowbuf, row_bytes) != 0) {
            close(fd);
            free(rowbuf);
            return -1;
        }
    }
    close(fd);
    free(rowbuf);
    return 0;
}

static int save_image(const char *raw)
{
    char path[MAX_PATH];
    unsigned char head[KPX_HEADER];
    unsigned long total, done = 0;
    int fd;

    resolve_path(raw, path, sizeof(path));
    if (!path[0])
        return -1;
    if (ends_with_ci(path, ".bmp"))
        return save_bmp(path);

    total = (unsigned long)canvas.w * (unsigned long)canvas.h;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    memcpy(head, KPX_MAGIC, 4);
    put_le32(head + 4, (uint32_t)canvas.w);
    put_le32(head + 8, (uint32_t)canvas.h);
    put_le32(head + 12, 0);
    if (write_all_fd(fd, head, sizeof(head)) != 0) {
        close(fd);
        return -1;
    }
    while (done < total) {
        unsigned long chunk = total - done;
        long n;
        if (chunk > 4096)
            chunk = 4096;
        n = write(fd, canvas.px + done, chunk * sizeof(uint32_t));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (unsigned long)n / sizeof(uint32_t);
    }
    close(fd);
    return 0;
}

static int load_bmp_fd(int fd, const unsigned char head54[54])
{
    uint32_t off = get_le32(head54 + 10);
    uint32_t hdr = get_le32(head54 + 14);
    int32_t w = (int32_t)get_le32(head54 + 18);
    int32_t h = (int32_t)get_le32(head54 + 22);
    uint16_t bpp = get_le16(head54 + 28);
    uint32_t comp = get_le32(head54 + 30);
    int top_down = 0;
    unsigned long stride;
    unsigned char *rowbuf;

    if (hdr < 40 || (bpp != 24 && bpp != 32) || comp != 0)
        return -2;
    if (h < 0) {
        top_down = 1;
        h = -h;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || off < 54)
        return -2;
    if (seek(fd, (long)off, SEEK_SET) < 0)
        return -2;

    stride = (((unsigned long)w * (unsigned long)bpp + 31UL) / 32UL) * 4UL;
    rowbuf = malloc(stride);
    if (!rowbuf)
        return -1;

    undo_push();
    canvas_clear();
    for (int32_t row = 0; row < h; row++) {
        unsigned long got = 0;
        int32_t dy = top_down ? row : (h - 1 - row);
        int32_t copy_w = w < canvas.w ? w : canvas.w;

        while (got < stride) {
            long n = read(fd, rowbuf + got, stride - got);
            if (n <= 0)
                break;
            got += (unsigned long)n;
        }
        if (got < stride)
            break;
        if (dy < 0 || dy >= canvas.h)
            continue;
        for (int32_t x = 0; x < copy_w; x++) {
            int p = x * (bpp / 8);
            canvas.px[(long)dy * canvas.w + x] =
                GUI_RGB(rowbuf[p + 2], rowbuf[p + 1], rowbuf[p + 0]);
        }
    }
    free(rowbuf);
    return 0;
}

static int load_image(const char *raw)
{
    char path[MAX_PATH];
    unsigned char head[54];
    long r;
    uint32_t w, h;
    int fd;

    resolve_path(raw, path, sizeof(path));
    if (!path[0])
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    r = read(fd, head, KPX_HEADER);
    if (r < KPX_HEADER) {
        close(fd);
        return -2;
    }
    if (head[0] == 'B' && head[1] == 'M') {
        long r2 = read(fd, head + KPX_HEADER, 54 - KPX_HEADER);
        int rc;
        if (r2 != 54 - KPX_HEADER) {
            close(fd);
            return -2;
        }
        rc = load_bmp_fd(fd, head);
        close(fd);
        return rc;
    }
    if (memcmp(head, KPX_MAGIC, 4) != 0) {
        close(fd);
        return -2;
    }
    w = get_le32(head + 4);
    h = get_le32(head + 8);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        close(fd);
        return -2;
    }

    {
        uint32_t *line = malloc(w * sizeof(uint32_t));
        if (!line) {
            close(fd);
            return -1;
        }
        undo_push();
        canvas_clear();
        for (unsigned long row = 0; row < h && row < (unsigned long)canvas.h;
             row++) {
            unsigned long want = w * sizeof(uint32_t);
            unsigned long got = 0;
            unsigned long copy = w < (uint32_t)canvas.w ? w
                                                        : (uint32_t)canvas.w;

            while (got < want) {
                long n = read(fd, (char *)line + got, want - got);
                if (n <= 0)
                    break;
                got += (unsigned long)n;
            }
            if (got < want)
                break;
            memcpy(canvas.px + row * canvas.w, line, copy * sizeof(uint32_t));
        }
        free(line);
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------- Talon helpers */

static int parse_hex_color(const char *s, uint32_t *out)
{
    int len;

    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    len = 0;
    while (s[len] && s[len] != ' ' && s[len] != '\t' &&
           s[len] != '\r' && s[len] != '\n')
        len++;
    if (len == 6) {
        uint32_t c = 0;
        for (int i = 0; i < 6; i++) {
            int hv = hex_val(s[i]);
            if (hv < 0)
                return -1;
            c = (c << 4) | (uint32_t)hv;
        }
        *out = c;
        return 0;
    }
    if (len == 3) {
        int r = hex_val(s[0]), g = hex_val(s[1]), b = hex_val(s[2]);
        if (r < 0 || g < 0 || b < 0)
            return -1;
        *out = GUI_RGB(r * 17, g * 17, b * 17);
        return 0;
    }
    return -1;
}

static int talon_slot0_hex(uint32_t *out_col)
{
    struct k_parcel_info info;
    char buf[64];
    long n;

    if (parcel_list(0, &info) != 0)
        return 0;
    if (info.type != K_PARCEL_COLOR && info.type != K_PARCEL_TEXT)
        return 0;
    if (info.size == 0 || info.size >= sizeof(buf))
        return 0;
    n = parcel_get(0, &info, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return parse_hex_color(buf, out_col) == 0;
}

/* ------------------------------------------------------------ painting */

static void visible_canvas_size(const gui_window *win, int *vw, int *vh)
{
    int w = win->w - CANVAS_X;
    int h = win->h - CANVAS_Y - STATUS_H;

    *vw = w > 0 ? w : 0;
    *vh = h > 0 ? h : 0;
}

static void blit_canvas(gui_window *win, int x, int y, int w, int h)
{
    int vw, vh;

    visible_canvas_size(win, &vw, &vh);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas.w) w = canvas.w - x;
    if (y + h > canvas.h) h = canvas.h - y;
    if (x + w > vw) w = vw - x;
    if (y + h > vh) h = vh - y;
    if (w <= 0 || h <= 0)
        return;
    gui_bitmap(win, CANVAS_X + x, CANVAS_Y + y, w, h,
               canvas.px + (long)y * canvas.w + x, canvas.w);
}

static void fill_canvas_margins(gui_window *win)
{
    int vw, vh;

    visible_canvas_size(win, &vw, &vh);
    if (vw > canvas.w)
        gui_rect(win, CANVAS_X + canvas.w, CANVAS_Y, vw - canvas.w, vh,
                 GUI_SUNKEN);
    if (vh > canvas.h)
        gui_rect(win, CANVAS_X, CANVAS_Y + canvas.h, vw, vh - canvas.h,
                 GUI_SUNKEN);
}

static void draw_status(gui_window *win)
{
    int y = win->h - STATUS_H;

    if (y < 0)
        return;
    gui_rect(win, 0, y, win->w, STATUS_H, GUI_PANEL);
    gui_rect(win, 0, y, win->w, 1, GUI_BORDER);
    gui_push_clip_xy(win, 8, y, win->w - 16, STATUS_H);
    gui_text(win, 8, y + (STATUS_H - GUI_FONT_H) / 2, status, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_pop_clip(win);
}

static void set_status_default(void)
{
    snprintf(status, sizeof(status),
             "%dx%d   %s   brush %d   #%02X%02X%02X   undo:%d   KPX1/BMP",
             canvas.w, canvas.h, tool_names[tool], brushes[brush],
             GUI_RED(colour), GUI_GREEN(colour), GUI_BLUE(colour),
             undo_count);
}

static void draw_chrome(gui_window *win, gui_ui *ui, int *action)
{
    int w = win->w, h = win->h;
    int y, i;
    int name_w = w - 46 - 294;
    uint32_t slot0_col = 0;
    int has_talon_col;
    char hexstr[16];

    if (name_w < 60)
        name_w = 60;

    /* Top bar */
    gui_rect(win, 0, 0, w, TOP_H, GUI_PANEL);
    gui_rect(win, 0, TOP_H - 1, w, 1, GUI_BORDER);
    gui_text(win, 8, (TOP_H - GUI_FONT_H) / 2, "file", GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    if (gui_textbox(win, gui_mkrc(46, 7, name_w, 26), &namebox, ui))
        *action = 1;
    if (gui_button(win, gui_mkrc(w - 286, 7, 64, 26), "Save", ui))
        *action = 1;
    if (gui_button(win, gui_mkrc(w - 216, 7, 64, 26), "Load", ui))
        *action = 2;
    if (gui_button_ex(win, gui_mkrc(w - 146, 7, 72, 26), "Undo", ui,
                      undo_count > 0, 0))
        *action = 4;
    if (gui_button(win, gui_mkrc(w - 68, 7, 60, 26), "Clear", ui))
        *action = 3;

    /* Left tool sidebar */
    gui_rect(win, 0, TOP_H, TOOL_W, h - TOP_H - STATUS_H, GUI_PANEL);
    gui_rect(win, TOOL_W - 1, TOP_H, 1, h - TOP_H - STATUS_H, GUI_BORDER);
    gui_push_clip_xy(win, 0, TOP_H, TOOL_W - 1, h - TOP_H - STATUS_H);

    /* Swatches */
    y = TOP_H + 8;
    for (i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        gui_rc r = gui_mkrc(8 + col * 26, y + row * 24, 22, 20);

        gui_rect(win, r.x, r.y, r.w, r.h, swatches[i]);
        gui_frame(win, r.x, r.y, r.w, r.h,
                  swatches[i] == colour && tool != TOOL_ERASER ? GUI_ACCENT
                                                               : GUI_BORDER);
        if (swatches[i] == colour && tool != TOOL_ERASER)
            gui_frame(win, r.x - 1, r.y - 1, r.w + 2, r.h + 2, GUI_ACCENT);
        if (ui->up && gui_hit(r, ui->up_x, ui->up_y) &&
            gui_hit(r, ui->down_x, ui->down_y)) {
            colour = swatches[i];
            if (tool == TOOL_ERASER)
                tool = TOOL_PEN;
            set_status_default();
        }
    }

    /* Active colour preview + Talon Hex buttons */
    y += 4 * 24 + 8;
    snprintf(hexstr, sizeof(hexstr), "#%02X%02X%02X",
             GUI_RED(colour), GUI_GREEN(colour), GUI_BLUE(colour));
    gui_rect(win, 8, y, 22, 22, colour);
    gui_frame(win, 8, y, 22, 22, GUI_EDGE);
    gui_text(win, 36, y + 3, hexstr, GUI_TEXT, GUI_TRANSPARENT);

    y += 28;
    if (gui_button(win, gui_mkrc(8, y, 100, 24), "Copy Hex", ui)) {
        if (parcel_put(K_PARCEL_COLOR, "text/x-color", hexstr, hexstr, 7) >= 0)
            snprintf(status, sizeof(status), "Copied %s to Talon", hexstr);
        else
            snprintf(status, sizeof(status), "Talon copy failed");
    }

    y += 28;
    has_talon_col = talon_slot0_hex(&slot0_col);
    if (gui_button_ex(win, gui_mkrc(8, y, 100, 24), "Pick Talon", ui,
                      has_talon_col, 0)) {
        colour = slot0_col;
        if (tool == TOOL_ERASER)
            tool = TOOL_PEN;
        snprintf(status, sizeof(status), "Picked #%02X%02X%02X from Talon",
                 GUI_RED(colour), GUI_GREEN(colour), GUI_BLUE(colour));
    }

    /* Brush sizes */
    y += 32;
    gui_text(win, 10, y, "brush", GUI_TEXT_DIM, GUI_TRANSPARENT);
    y += GUI_FONT_H + 3;
    for (i = 0; i < 5; i++) {
        char lab[8];
        gui_rc r = gui_mkrc(8 + (i % 3) * 34, y + (i / 3) * 26, 30, 22);
        snprintf(lab, sizeof(lab), "%d", brushes[i]);
        if (gui_button_ex(win, r, lab, ui, 1, i == brush)) {
            brush = i;
            set_status_default();
        }
    }

    /* Tools */
    y += 2 * 26 + 8;
    gui_text(win, 10, y, "tools", GUI_TEXT_DIM, GUI_TRANSPARENT);
    y += GUI_FONT_H + 3;
    for (i = 0; i < TOOL_COUNT; i++) {
        gui_rc r = gui_mkrc(8 + (i % 2) * 52, y + (i / 2) * 26, 48, 22);
        if (gui_button_ex(win, r, tool_names[i], ui, 1, i == tool)) {
            tool = i;
            set_status_default();
        }
    }

    y += ((TOOL_COUNT + 1) / 2) * 26 + 6;
    if (gui_checkbox(win, 10, y, "Filled", &shape_filled, ui))
        set_status_default();

    gui_pop_clip(win);
}

static void draw_preview_if_needed(gui_window *win)
{
    int vw, vh;

    if (!shape_drag)
        return;
    visible_canvas_size(win, &vw, &vh);
    gui_push_clip_xy(win, CANVAS_X, CANVAS_Y,
                     vw < canvas.w ? vw : canvas.w,
                     vh < canvas.h ? vh : canvas.h);
    draw_shape_to(win, CANVAS_X, CANVAS_Y, tool,
                  shape_x0, shape_y0, shape_x1, shape_y1,
                  colour, brushes[brush], shape_filled);
    gui_pop_clip(win);
}

static void full_repaint(gui_window *win, gui_ui *ui, int *action)
{
    draw_chrome(win, ui, action);
    fill_canvas_margins(win);
    blit_canvas(win, 0, 0, canvas.w, canvas.h);
    draw_preview_if_needed(win);
    draw_status(win);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *win;
    gui_ui ui;
    int dirty = 1, i, action = 0, had_file = 0;

    memset(&ui, 0, sizeof(ui));
    namebox.buf = namebuf;
    namebox.cap = sizeof(namebuf);
    namebox.len = (int)strlen(namebuf);
    namebox.caret = namebox.len;

    snprintf(g_cwd, sizeof(g_cwd), "%s", kestrel_startup_cwd());
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            snprintf(g_cwd, sizeof(g_cwd), "%s", argv[i] + 6);
            continue;
        }
        snprintf(namebuf, sizeof(namebuf), "%s", argv[i]);
        namebox.len = (int)strlen(namebuf);
        namebox.caret = namebox.len;
        had_file = 1;
    }

    canvas.px = malloc((unsigned long)CANVAS_W * CANVAS_H * sizeof(uint32_t));
    if (!canvas.px) {
        printf("gpaint: out of memory for a %dx%d canvas\n",
               CANVAS_W, CANVAS_H);
        return 1;
    }
    canvas.wid = 0;
    canvas.open = 0;
    canvas.w = CANVAS_W;
    canvas.h = CANVAS_H;
    canvas.clip = gui_mkrc(0, 0, CANVAS_W, CANVAS_H);
    canvas_clear();

    win = gui_open("Paint", 50, 36, INIT_W, INIT_H, 0);
    if (!win) {
        printf("gpaint: no window manager available\n");
        free(canvas.px);
        return 1;
    }

    if (had_file && load_image(namebuf) == 0) {
        undo_free_all();
        snprintf(status, sizeof(status), "loaded %s", namebuf);
    } else {
        set_status_default();
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, 16);
        int drew = 0, bx = 0, by = 0, bw = 0, bh = 0;

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;

            if (ev.type == KEV_KEY && !namebox.focus) {
                int k = (int)ev.key;
                int new_tool = -1;

                if (k == 'p')
                    new_tool = TOOL_PEN;
                else if (k == 'e')
                    new_tool = TOOL_ERASER;
                else if (k == 'f')
                    new_tool = TOOL_FILL;
                else if (k == 'l')
                    new_tool = TOOL_LINE;
                else if (k == 'r')
                    new_tool = TOOL_RECT;
                else if (k == 'c')
                    new_tool = TOOL_CIRCLE;
                else if (k == 'i')
                    new_tool = TOOL_PICK;

                if (new_tool >= 0) {
                    tool = new_tool;
                    set_status_default();
                } else if (k == '[') {
                    if (brush > 0) {
                        brush--;
                        set_status_default();
                    }
                } else if (k == ']') {
                    if (brush < (int)(sizeof(brushes) / sizeof(brushes[0])) - 1) {
                        brush++;
                        set_status_default();
                    }
                } else if (k == 26 || k == 'u') { /* Ctrl+Z or u */
                    if (undo_pop() == 0)
                        set_status_default();
                    else
                        snprintf(status, sizeof(status), "Nothing to undo");
                }
            }

            if (ev.type == KEV_MOUSE_DOWN || ev.type == KEV_MOUSE_MOVE ||
                ev.type == KEV_MOUSE_UP) {
                int vw, vh;
                int cxp = ev.x - CANVAS_X;
                int cyp = ev.y - CANVAS_Y;
                int inside;
                int down_in_canvas;

                visible_canvas_size(win, &vw, &vh);
                inside = cxp >= 0 && cyp >= 0 && cxp < canvas.w &&
                         cyp < canvas.h && cxp < vw && cyp < vh;
                down_in_canvas = ui.down_x >= CANVAS_X && ui.down_y >= CANVAS_Y &&
                                 ui.down_x < CANVAS_X + (vw < canvas.w ? vw : canvas.w) &&
                                 ui.down_y < CANVAS_Y + (vh < canvas.h ? vh : canvas.h);

                if (ev.type == KEV_MOUSE_DOWN && inside &&
                    (ev.buttons & (unsigned)K_MOUSE_RIGHT)) {
                    /* Right-click is a quick eyedropper with any tool. */
                    canvas_pick_colour(cxp, cyp);
                } else if (tool == TOOL_PEN || tool == TOOL_ERASER) {
                    int px = last_x, py = last_y;
                    if (ev.type == KEV_MOUSE_UP || !inside) {
                        last_x = -1;
                        last_y = -1;
                    }
                    if (inside &&
                        (ev.type == KEV_MOUSE_DOWN ||
                         ((ev.buttons & (unsigned)K_MOUSE_LEFT) && down_in_canvas))) {
                        uint32_t c = (tool == TOOL_ERASER) ? canvas_bg : colour;
                        int pad = brushes[brush] + 2;

                        if (ev.type == KEV_MOUSE_DOWN)
                            undo_push();
                        if (ev.type == KEV_MOUSE_DOWN || px < 0) {
                            canvas_dot(cxp, cyp, c);
                            px = cxp;
                            py = cyp;
                        } else {
                            canvas_stroke(px, py, cxp, cyp, c);
                        }
                        last_x = cxp;
                        last_y = cyp;
                        bx = (px < cxp ? px : cxp) - pad;
                        by = (py < cyp ? py : cyp) - pad;
                        bw = (px > cxp ? px - cxp : cxp - px) + 2 * pad;
                        bh = (py > cyp ? py - cyp : cyp - py) + 2 * pad;
                        snprintf(status, sizeof(status),
                                 "%d, %d   brush %d   %s   undo:%d", cxp, cyp,
                                 brushes[brush], tool_names[tool], undo_count);
                        drew = 1;
                    }
                } else if (tool == TOOL_FILL) {
                    if (ev.type == KEV_MOUSE_DOWN && inside) {
                        if (canvas_flood_fill(cxp, cyp, colour))
                            snprintf(status, sizeof(status),
                                     "Filled at %d,%d   undo:%d",
                                     cxp, cyp, undo_count);
                        blit_canvas(win, 0, 0, canvas.w, canvas.h);
                    }
                } else if (tool == TOOL_PICK) {
                    if (ev.type == KEV_MOUSE_DOWN && inside) {
                        canvas_pick_colour(cxp, cyp);
                        tool = TOOL_PEN;
                    }
                } else {
                    /* Shape tools: Line, Rect, Circle */
                    if (ev.type == KEV_MOUSE_DOWN && inside) {
                        shape_drag = 1;
                        shape_x0 = shape_x1 = cxp;
                        shape_y0 = shape_y1 = cyp;
                    } else if (shape_drag && ev.type == KEV_MOUSE_MOVE) {
                        int clx = cxp < 0 ? 0 : (cxp >= canvas.w ? canvas.w - 1 : cxp);
                        int cly = cyp < 0 ? 0 : (cyp >= canvas.h ? canvas.h - 1 : cyp);
                        shape_x1 = clx;
                        shape_y1 = cly;
                        snprintf(status, sizeof(status),
                                 "%s (%d,%d)->(%d,%d)", tool_names[tool],
                                 shape_x0, shape_y0, shape_x1, shape_y1);
                    } else if (shape_drag && ev.type == KEV_MOUSE_UP) {
                        undo_push();
                        draw_shape_to(&canvas, 0, 0, tool,
                                      shape_x0, shape_y0, shape_x1, shape_y1,
                                      colour, brushes[brush], shape_filled);
                        shape_drag = 0;
                        set_status_default();
                    }
                }
            }
            dirty = 1;
        } else if (namebox.focus) {
            dirty = 1;
        }

        if (drew) {
            blit_canvas(win, bx, by, bw, bh);
            draw_status(win);
            if (gui_flush(win) != 0)
                break;
            dirty = 0;
            continue;
        }

        if (!dirty)
            continue;

        action = 0;
        full_repaint(win, &ui, &action);
        if (action == 1) {
            if (save_image(namebuf) == 0)
                snprintf(status, sizeof(status), "saved %s (%dx%d)",
                         namebuf, canvas.w, canvas.h);
            else
                snprintf(status, sizeof(status), "cannot write %s", namebuf);
            draw_status(win);
        } else if (action == 2) {
            int rc = load_image(namebuf);
            if (rc == 0)
                snprintf(status, sizeof(status), "loaded %s", namebuf);
            else if (rc == -2)
                snprintf(status, sizeof(status), "%s is not a KPX1/BMP image",
                         namebuf);
            else
                snprintf(status, sizeof(status), "cannot read %s", namebuf);
            blit_canvas(win, 0, 0, canvas.w, canvas.h);
            draw_status(win);
        } else if (action == 3) {
            undo_push();
            canvas_clear();
            set_status_default();
            blit_canvas(win, 0, 0, canvas.w, canvas.h);
            draw_status(win);
        } else if (action == 4) {
            if (undo_pop() == 0)
                set_status_default();
            else
                snprintf(status, sizeof(status), "Nothing to undo");
            blit_canvas(win, 0, 0, canvas.w, canvas.h);
            draw_status(win);
        }

        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    undo_free_all();
    gui_close(win);
    free(canvas.px);
    return 0;
}
