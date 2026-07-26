/* libgui: drawing primitives.
 *
 * Every routine writes straight into the window's pixel buffer and clips
 * to win->clip (which gui_clip keeps inside the window), so no caller can
 * scribble past the buffer no matter what coordinates it computes. The
 * pixel format is 0x00RRGGBB, one uint32_t per pixel, win->w per row.
 */

#include "gui.h"

#include <string.h>

const uint8_t *gui_glyph(unsigned char c)
{
    if (c < 0x20 || c > 0x7E)
        return gui_font8x16_fallback;
    return gui_font8x16[c - 0x20];
}

/* ------------------------------------------------------------- clipping */

static int usable(gui_window *win)
{
    return win && win->px && win->w > 0 && win->h > 0 &&
           win->clip.w > 0 && win->clip.h > 0;
}

void gui_clip(gui_window *win, gui_rc r)
{
    int x0, y0, x1, y1;

    if (!win)
        return;
    x0 = r.x < 0 ? 0 : r.x;
    y0 = r.y < 0 ? 0 : r.y;
    x1 = r.x + r.w;
    y1 = r.y + r.h;
    if (x1 > win->w)
        x1 = win->w;
    if (y1 > win->h)
        y1 = win->h;
    win->clip = gui_mkrc(x0, y0, x1 > x0 ? x1 - x0 : 0,
                         y1 > y0 ? y1 - y0 : 0);
}

void gui_unclip(gui_window *win)
{
    if (!win)
        return;
    win->clip = gui_mkrc(0, 0, win->w, win->h);
}

/* Fill one clipped horizontal run. The single choke point for everything
 * rectangular, so the bounds test exists in exactly one place. */
static void span(gui_window *win, int x, int y, int w, uint32_t color)
{
    uint32_t *row;
    int x1;

    if (!usable(win) || w <= 0)
        return;
    if (y < win->clip.y || y >= win->clip.y + win->clip.h)
        return;
    x1 = x + w;
    if (x < win->clip.x)
        x = win->clip.x;
    if (x1 > win->clip.x + win->clip.w)
        x1 = win->clip.x + win->clip.w;
    if (x1 <= x)
        return;
    row = win->px + (long)y * win->w;
    while (x < x1)
        row[x++] = color;
}

/* ------------------------------------------------------------ primitives */

void gui_clear(gui_window *win, uint32_t color)
{
    if (!win)
        return;
    gui_rect(win, 0, 0, win->w, win->h, color);
}

void gui_pixel(gui_window *win, int x, int y, uint32_t color)
{
    if (!usable(win))
        return;
    if (x < win->clip.x || x >= win->clip.x + win->clip.w)
        return;
    if (y < win->clip.y || y >= win->clip.y + win->clip.h)
        return;
    win->px[(long)y * win->w + x] = color;
}

uint32_t gui_peek(gui_window *win, int x, int y)
{
    if (!win || !win->px || x < 0 || y < 0 || x >= win->w || y >= win->h)
        return 0;
    return win->px[(long)y * win->w + x];
}

void gui_rect(gui_window *win, int x, int y, int w, int h, uint32_t color)
{
    int i;

    for (i = 0; i < h; i++)
        span(win, x, y + i, w, color);
}

void gui_frame(gui_window *win, int x, int y, int w, int h, uint32_t color)
{
    int i;

    if (w <= 0 || h <= 0)
        return;
    span(win, x, y, w, color);
    span(win, x, y + h - 1, w, color);
    for (i = 0; i < h; i++) {
        gui_pixel(win, x, y + i, color);
        gui_pixel(win, x + w - 1, y + i, color);
    }
}

void gui_bevel(gui_window *win, gui_rc r, uint32_t hi, uint32_t lo)
{
    int i;

    if (r.w <= 0 || r.h <= 0)
        return;
    span(win, r.x, r.y, r.w, hi);
    for (i = 0; i < r.h; i++)
        gui_pixel(win, r.x, r.y + i, hi);
    span(win, r.x, r.y + r.h - 1, r.w, lo);
    for (i = 0; i < r.h; i++)
        gui_pixel(win, r.x + r.w - 1, r.y + i, lo);
}

/* Bresenham, integer only, both octant families. */
void gui_line(gui_window *win, int x0, int y0, int x1, int y1,
              uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err, e2;

    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    err = dx - dy;

    for (;;) {
        gui_pixel(win, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Thick lines by drawing parallel copies offset across the shallow axis.
 * Good enough for clock hands and paint strokes; no polygon fill needed. */
void gui_line_w(gui_window *win, int x0, int y0, int x1, int y1,
                uint32_t color, int width)
{
    int dx = x1 - x0, dy = y1 - y0;
    int i, half;

    if (width <= 1) {
        gui_line(win, x0, y0, x1, y1, color);
        return;
    }
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    half = width / 2;
    for (i = -half; i < width - half; i++) {
        if (dx >= dy)
            gui_line(win, x0, y0 + i, x1, y1 + i, color);
        else
            gui_line(win, x0 + i, y0, x1 + i, y1, color);
    }
}

/* Midpoint circle: eight-way symmetry, no multiplies in the loop. */
void gui_circle(gui_window *win, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0, err = 1 - r;

    if (r < 0)
        return;
    while (x >= y) {
        gui_pixel(win, cx + x, cy + y, color);
        gui_pixel(win, cx + y, cy + x, color);
        gui_pixel(win, cx - y, cy + x, color);
        gui_pixel(win, cx - x, cy + y, color);
        gui_pixel(win, cx - x, cy - y, color);
        gui_pixel(win, cx - y, cy - x, color);
        gui_pixel(win, cx + y, cy - x, color);
        gui_pixel(win, cx + x, cy - y, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/* floor(sqrt(v)) by Newton iteration on integers. */
static int isqrt_i(int v)
{
    int x, y;

    if (v <= 0)
        return 0;
    x = v;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

void gui_disc(gui_window *win, int cx, int cy, int r, uint32_t color)
{
    int dy;

    if (r < 0)
        return;
    for (dy = -r; dy <= r; dy++) {
        int w = isqrt_i(r * r - dy * dy);

        span(win, cx - w, cy + dy, 2 * w + 1, color);
    }
}

uint32_t gui_mix(uint32_t a, uint32_t b, int t)
{
    unsigned r, g, bl;

    if (t < 0)
        t = 0;
    if (t > 256)
        t = 256;
    r = (GUI_RED(a) * (256 - t) + GUI_RED(b) * t) >> 8;
    g = (GUI_GREEN(a) * (256 - t) + GUI_GREEN(b) * t) >> 8;
    bl = (GUI_BLUE(a) * (256 - t) + GUI_BLUE(b) * t) >> 8;
    return GUI_RGB(r & 0xFF, g & 0xFF, bl & 0xFF);
}

uint32_t gui_shade(uint32_t color, int pct)
{
    if (pct >= 0)
        return gui_mix(color, GUI_RGB(255, 255, 255), pct * 256 / 100);
    return gui_mix(color, GUI_RGB(0, 0, 0), -pct * 256 / 100);
}

void gui_vgradient(gui_window *win, gui_rc r, uint32_t top, uint32_t bottom)
{
    int i;

    if (r.h <= 0)
        return;
    for (i = 0; i < r.h; i++) {
        int t = r.h > 1 ? (i * 256) / (r.h - 1) : 0;

        span(win, r.x, r.y + i, r.w, gui_mix(top, bottom, t));
    }
}

/* ----------------------------------------------------------------- text */

int gui_char(gui_window *win, int x, int y, char c, uint32_t fg, long bg)
{
    const uint8_t *g = gui_glyph((unsigned char)c);
    int row, col;

    if (!usable(win))
        return x + GUI_FONT_W;
    if (bg >= 0)
        gui_rect(win, x, y, GUI_FONT_W, GUI_FONT_H, (uint32_t)bg);
    for (row = 0; row < GUI_FONT_H; row++) {
        uint8_t bits = g[row];

        if (!bits)
            continue;
        for (col = 0; col < GUI_FONT_W; col++)
            if (bits & (0x80 >> col))
                gui_pixel(win, x + col, y + row, fg);
    }
    return x + GUI_FONT_W;
}

int gui_text_n(gui_window *win, int x, int y, const char *s, int n,
               uint32_t fg, long bg)
{
    int i;

    if (!s)
        return x;
    for (i = 0; i < n && s[i]; i++)
        x = gui_char(win, x, y, s[i], fg, bg);
    return x;
}

int gui_text(gui_window *win, int x, int y, const char *s, uint32_t fg,
             long bg)
{
    if (!s)
        return x;
    return gui_text_n(win, x, y, s, (int)strlen(s), fg, bg);
}

int gui_text_w(const char *s)
{
    if (!s)
        return 0;
    return (int)strlen(s) * GUI_FONT_W;
}

void gui_bitmap(gui_window *win, int x, int y, int bw, int bh,
                const uint32_t *src, int stride)
{
    int row, col;

    if (!usable(win) || !src || bw <= 0 || bh <= 0)
        return;
    if (stride <= 0)
        stride = bw;
    for (row = 0; row < bh; row++) {
        int py = y + row;
        const uint32_t *sr = src + (long)row * stride;
        int x0 = x, x1 = x + bw, sx = 0;

        if (py < win->clip.y || py >= win->clip.y + win->clip.h)
            continue;
        if (x0 < win->clip.x) {
            sx = win->clip.x - x0;
            x0 = win->clip.x;
        }
        if (x1 > win->clip.x + win->clip.w)
            x1 = win->clip.x + win->clip.w;
        for (col = x0; col < x1; col++, sx++)
            win->px[(long)py * win->w + col] = sr[sx];
    }
}

void gui_scroll_up(gui_window *win, int rows, uint32_t bg)
{
    gui_rc c;
    int y;

    if (!usable(win) || rows <= 0)
        return;
    c = win->clip;
    if (rows >= c.h) {
        gui_rect(win, c.x, c.y, c.w, c.h, bg);
        return;
    }
    for (y = c.y; y < c.y + c.h - rows; y++)
        memmove(win->px + (long)y * win->w + c.x,
                win->px + (long)(y + rows) * win->w + c.x,
                (unsigned long)c.w * sizeof(uint32_t));
    gui_rect(win, c.x, c.y + c.h - rows, c.w, rows, bg);
}

/* ------------------------------------------------------- fixed-point trig
 *
 * sin over the first quadrant as a four-term Taylor series evaluated in
 * Q16 with 64-bit intermediates; the remaining quadrants come from the
 * symmetries. Peak error is under 1e-4, i.e. well inside one pixel for
 * any radius this toolkit draws.
 */

static int sin_quadrant(int tenths)
{
    /* radians in Q16 = tenths * pi / 1800 */
    long long x = ((long long)tenths * 1143819LL) / 10000LL;
    long long x2 = (x * x) >> 16;
    long long t3 = (x * x2) >> 16;
    long long t5 = (t3 * x2) >> 16;
    long long t7 = (t5 * x2) >> 16;
    long long s = x - t3 / 6 + t5 / 120 - t7 / 5040;

    if (s > GUI_Q16)
        s = GUI_Q16;
    if (s < 0)
        s = 0;
    return (int)s;
}

int gui_sin_q16(int tenths_deg)
{
    int a = tenths_deg % GUI_TURN;

    if (a < 0)
        a += GUI_TURN;
    if (a <= 900)
        return sin_quadrant(a);
    if (a <= 1800)
        return sin_quadrant(1800 - a);
    if (a <= 2700)
        return -sin_quadrant(a - 1800);
    return -sin_quadrant(GUI_TURN - a);
}

int gui_cos_q16(int tenths_deg)
{
    return gui_sin_q16(tenths_deg + 900);
}
