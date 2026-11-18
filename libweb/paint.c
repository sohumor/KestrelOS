/* paint.c - drawing a laid-out page.
 *
 * Everything here is clipped twice: once against the dirty rectangle the
 * caller asked for, once against the box's own clip rectangle (which
 * layout already intersected with every clipping ancestor). A box that
 * misses either is not drawn at all, which is what makes scrolling cost
 * a strip rather than a screen.
 */

#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "css.h"
#include "font.h"
#include "img.h"
#include "layout.h"
#include "paint.h"

/* ---------------------------------------------------------------- util */

static int32_t pmin(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t pmax(int32_t a, int32_t b) { return a > b ? a : b; }

uint32_t paint_blend(uint32_t bg, uint32_t fg, unsigned a)
{
    unsigned r, g, b;

    if (a >= 255)
        return fg & 0x00FFFFFFu;
    if (!a)
        return bg & 0x00FFFFFFu;
    r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255 - a)) / 255;
    g = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * (255 - a)) / 255;
    b = ((fg & 0xFF) * a + (bg & 0xFF) * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

static uint32_t shade(uint32_t c, int pct)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF);
    int b = (int)(c & 0xFF);

    r += r * pct / 100;
    g += g * pct / 100;
    b += b * pct / 100;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int32_t tstride(const struct paint_target *t)
{
    return t->stride > 0 ? t->stride : t->w;
}

void paint_fill(const struct paint_target *t, lay_rect clip, lay_rect r,
                uint32_t color)
{
    int32_t x0, y0, x1, y1, x, y, st;
    unsigned a = (color >> 24) & 0xFFu;

    if (!t || !t->px || !a)
        return;
    r = lay_rect_intersect(r, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    st = tstride(t);
    if (a >= 255) {
        uint32_t c = color & 0x00FFFFFFu;

        for (y = y0; y < y1; y++) {
            uint32_t *row = t->px + (long)y * st;

            for (x = x0; x < x1; x++)
                row[x] = c;
        }
        return;
    }
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;

        for (x = x0; x < x1; x++)
            row[x] = paint_blend(row[x], color, a);
    }
}

/* ---------------------------------------------------------------- SVG
 *
 * Inline SVG is kept deliberately bounded and painted directly into the
 * page surface.  This first vector slice implements the interoperable core
 * used by icons and diagrams: nested groups, basic shapes, bounded path
 * flattening, text, presentation fill/stroke attributes, and viewBox scaling.
 * Unknown elements are skipped without hiding their recognised descendants.
 */

#define SVG_SHAPE_MAX   512
#define SVG_PIXEL_MAX   (4UL * 1024UL * 1024UL)
#define SVG_WORK_MAX    (4UL * 1024UL * 1024UL)
#define SVG_COORD_MAX   1000000000LL
#define SVG_ELLIPSE_SCALE 1048576LL
#define SVG_POINT_MAX   1024
#define SVG_SUBPATH_MAX 64

struct svg_painter {
    const struct paint_target *t;
    lay_rect clip, dst;
    int64_t minx, miny, vieww, viewh; /* thousandths of a user unit */
    const struct dom_node *root;
    uint32_t current_color;
    unsigned opacity;            /* 0..1000 */
    int shapes;
    unsigned long pixels, work;
};

struct svg_style {
    uint32_t fill, stroke;       /* AARRGGBB; alpha zero means none */
    uint32_t current_color;      /* AARRGGBB */
    int32_t stroke_width;        /* thousandths of a user unit */
};

struct svg_subpath {
    int start;
    int count;
    uint8_t closed;
};

static int32_t svg_sat_coord(int64_t v)
{
    if (v > SVG_COORD_MAX) return (int32_t)SVG_COORD_MAX;
    if (v < -SVG_COORD_MAX) return (int32_t)-SVG_COORD_MAX;
    return (int32_t)v;
}

static int32_t svg_sat_add(int32_t a, int32_t b)
{
    return svg_sat_coord((int64_t)a + b);
}

static int64_t svg_max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int64_t svg_min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static int svg_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int svg_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return !*a && !*b;
}

static int svg_ieq_n(const char *a, unsigned long n, const char *b)
{
    unsigned long i = 0;

    while (i < n && b[i]) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
        i++;
    }
    return i == n && !b[i];
}

static uint32_t svg_color(const char *s, uint32_t fallback,
                          uint32_t current_color)
{
    static const struct {
        const char *name;
        uint32_t rgb;
    } named[] = {
        { "black",   0x000000u }, { "white",   0xFFFFFFu },
        { "red",     0xFF0000u }, { "green",   0x008000u },
        { "blue",    0x0000FFu }, { "yellow",  0xFFFF00u },
        { "gray",    0x808080u }, { "grey",    0x808080u },
        { "orange",  0xFFA500u }, { "purple",  0x800080u },
        { "lime",    0x00FF00u }, { "navy",    0x000080u },
        { "teal",    0x008080u }, { "aqua",    0x00FFFFu },
        { "fuchsia", 0xFF00FFu }, { "maroon",  0x800000u }
    };
    const char *end;
    unsigned long i, len;

    if (!s || !*s)
        return fallback;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        end--;
    len = (unsigned long)(end - s);
    if (!len)
        return fallback;
    if (svg_ieq_n(s, len, "none") || svg_ieq_n(s, len, "transparent"))
        return 0;
    if (svg_ieq_n(s, len, "currentColor"))
        return 0xFF000000u | (current_color & 0x00FFFFFFu);
    if (*s == '#') {
        int a, b, c, d, e, f;
        unsigned long n = len - 1;

        s++;
        if (n != 3 && n != 6)
            return fallback;
        a = svg_hex(s[0]); b = svg_hex(s[1]); c = svg_hex(s[2]);
        if (a < 0 || b < 0 || c < 0)
            return fallback;
        if (n == 3)
            return 0xFF000000u | (uint32_t)(a * 17) << 16 |
                   (uint32_t)(b * 17) << 8 | (uint32_t)(c * 17);
        d = svg_hex(s[3]); e = svg_hex(s[4]); f = svg_hex(s[5]);
        if (d < 0 || e < 0 || f < 0)
            return fallback;
        return 0xFF000000u | (uint32_t)((a << 4) | b) << 16 |
               (uint32_t)((c << 4) | d) << 8 |
               (uint32_t)((e << 4) | f);
    }
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (svg_ieq_n(s, len, named[i].name))
            return 0xFF000000u | named[i].rgb;
    return fallback;
}

static int svg_style_value(const struct dom_node *n, const char *property,
                           char *out, unsigned long outsz)
{
    const char *style = dom_get_attr(n, "style");
    unsigned long pn = strlen(property);

    if (!style || !outsz)
        return 0;
    while (*style) {
        const char *name, *name_end, *value, *value_end;
        unsigned long i, len;

        while (*style == ' ' || *style == '\t' || *style == ';') style++;
        name = style;
        while (*style && *style != ':' && *style != ';') style++;
        name_end = style;
        while (name_end > name &&
               (name_end[-1] == ' ' || name_end[-1] == '\t'))
            name_end--;
        if (*style != ':') {
            while (*style && *style != ';') style++;
            continue;
        }
        style++;
        while (*style == ' ' || *style == '\t') style++;
        value = style;
        while (*style && *style != ';') style++;
        value_end = style;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t'))
            value_end--;
        if ((unsigned long)(name_end - name) != pn)
            continue;
        for (i = 0; i < pn; i++) {
            int a = name[i], b = property[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (i != pn)
            continue;
        len = (unsigned long)(value_end - value);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, value, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

static int svg_number(const char **pp, int32_t *out)
{
    const char *p = *pp;
    int64_t whole = 0, frac = 0, scale = 1;
    int sign = 1, have = 0, digits = 0;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
           *p == ',')
        p++;
    if (*p == '-' || *p == '+') {
        if (*p++ == '-') sign = -1;
    }
    while (*p >= '0' && *p <= '9') {
        have = 1;
        if (whole < 1000000)
            whole = whole * 10 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            have = 1;
            if (digits++ < 3) {
                frac = frac * 10 + (*p - '0');
                scale *= 10;
            }
            p++;
        }
    }
    if (!have)
        return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == 'p' && p[1] == 'x') p += 2;
    whole = whole * 1000 + frac * 1000 / scale;
    if (whole > 1000000000LL) whole = 1000000000LL;
    *out = (int32_t)(sign * whole);
    *pp = p;
    return 1;
}

static int32_t svg_attr_num(const struct dom_node *n, const char *name,
                            int32_t def)
{
    const char *s = dom_get_attr(n, name);
    int32_t v;

    return s && svg_number(&s, &v) ? v : def;
}

static int32_t svg_x(const struct svg_painter *s, int32_t x)
{
    int64_t scaled = ((int64_t)x - s->minx) * s->dst.w / s->vieww;

    return svg_sat_coord((int64_t)s->dst.x + scaled);
}

static int32_t svg_y(const struct svg_painter *s, int32_t y)
{
    int64_t scaled = ((int64_t)y - s->miny) * s->dst.h / s->viewh;

    return svg_sat_coord((int64_t)s->dst.y + scaled);
}

static int32_t svg_w(const struct svg_painter *s, int32_t w)
{
    int64_t v = (int64_t)w * s->dst.w / s->vieww;

    if (v < 0) v = -v;
    return svg_sat_coord(v);
}

static int32_t svg_h(const struct svg_painter *s, int32_t h)
{
    int64_t v = (int64_t)h * s->dst.h / s->viewh;

    if (v < 0) v = -v;
    return svg_sat_coord(v);
}

static void svg_pixel(struct svg_painter *s, int64_t x, int64_t y,
                      uint32_t color)
{
    int32_t st;
    uint32_t *at;
    unsigned a;

    if (!color || s->work >= SVG_WORK_MAX)
        return;
    s->work++;
    if (s->pixels >= SVG_PIXEL_MAX ||
        x < s->clip.x || y < s->clip.y ||
        x >= s->clip.x + s->clip.w || y >= s->clip.y + s->clip.h ||
        x < 0 || y < 0 || x >= s->t->w || y >= s->t->h)
        return;
    a = color >> 24;
    if (s->opacity < 1000)
        a = (unsigned)((uint64_t)a * s->opacity / 1000);
    if (!a)
        return;
    s->pixels++;
    st = tstride(s->t);
    at = s->t->px + (long)y * st + (long)x;
    *at = a >= 255 ? color & 0xFFFFFFu : paint_blend(*at, color, a);
}

static void svg_fill_rect(struct svg_painter *s, lay_rect r, uint32_t color)
{
    int64_t x0, y0, x1, y1, x, y;

    if (!color || r.w <= 0 || r.h <= 0 ||
        s->clip.w <= 0 || s->clip.h <= 0)
        return;
    x0 = svg_max64(s->clip.x, r.x);
    y0 = svg_max64(s->clip.y, r.y);
    x1 = svg_min64((int64_t)s->clip.x + s->clip.w,
                   (int64_t)r.x + r.w);
    y1 = svg_min64((int64_t)s->clip.y + s->clip.h,
                   (int64_t)r.y + r.h);
    for (y = y0; y < y1 && s->work < SVG_WORK_MAX; y++)
        for (x = x0; x < x1 && s->work < SVG_WORK_MAX; x++)
            svg_pixel(s, x, y, color);
}

static void svg_line(struct svg_painter *s, int32_t x0, int32_t y0,
                     int32_t x1, int32_t y1, int32_t width,
                     uint32_t color)
{
    int64_t ax = x0, ay = y0, bx = x1, by = y1;
    int64_t dx = bx > ax ? bx - ax : ax - bx;
    int64_t sx = ax < bx ? 1 : -1;
    int64_t dy = by > ay ? ay - by : by - ay;
    int64_t sy = ay < by ? 1 : -1;
    int64_t err = dx + dy;
    int64_t r = width > 1 ? width / 2 : 0;

    if (!color || width <= 0 || s->work >= SVG_WORK_MAX)
        return;
    for (;;) {
        int64_t x, y, twice;

        for (y = -r; y <= r && s->work < SVG_WORK_MAX; y++)
            for (x = -r; x <= r && s->work < SVG_WORK_MAX; x++)
                svg_pixel(s, ax + x, ay + y, color);
        if ((ax == bx && ay == by) || s->work >= SVG_WORK_MAX)
            break;
        twice = 2 * err;
        if (twice >= dy) { err += dy; ax += sx; }
        if (twice <= dx) { err += dx; ay += sy; }
    }
}

static void svg_ellipse(struct svg_painter *s, int32_t cx, int32_t cy,
                        int32_t rx, int32_t ry, int32_t stroke,
                        uint32_t fill, uint32_t line)
{
    int64_t x0, x1, y0, y1, x, y, irx, iry;

    if (rx <= 0 || ry <= 0 || (!fill && !line) ||
        s->work >= SVG_WORK_MAX)
        return;
    x0 = svg_max64(s->clip.x, (int64_t)cx - rx);
    x1 = svg_min64((int64_t)s->clip.x + s->clip.w,
                   (int64_t)cx + rx + 1);
    y0 = svg_max64(s->clip.y, (int64_t)cy - ry);
    y1 = svg_min64((int64_t)s->clip.y + s->clip.h,
                   (int64_t)cy + ry + 1);
    irx = (int64_t)rx - stroke;
    iry = (int64_t)ry - stroke;
    for (y = y0; y < y1 && s->work < SVG_WORK_MAX; y++)
        for (x = x0; x < x1 && s->work < SVG_WORK_MAX; x++) {
            int64_t dx = x - (int64_t)cx;
            int64_t dy = y - (int64_t)cy;
            int64_t adx = dx < 0 ? -dx : dx;
            int64_t ady = dy < 0 ? -dy : dy;
            int outer = 0, inner = 0;

            if (adx <= rx && ady <= ry) {
                int64_t nx = adx * SVG_ELLIPSE_SCALE / rx;
                int64_t ny = ady * SVG_ELLIPSE_SCALE / ry;

                outer = nx * nx + ny * ny <=
                        SVG_ELLIPSE_SCALE * SVG_ELLIPSE_SCALE;
            }
            if (outer && irx > 0 && iry > 0 &&
                adx <= irx && ady <= iry) {
                int64_t nx = adx * SVG_ELLIPSE_SCALE / irx;
                int64_t ny = ady * SVG_ELLIPSE_SCALE / iry;

                inner = nx * nx + ny * ny <=
                        SVG_ELLIPSE_SCALE * SVG_ELLIPSE_SCALE;
            }

            s->work++;
            if (!outer)
                continue;
            if (line && stroke > 0 && !inner)
                svg_pixel(s, x, y, line);
            else if (fill && (stroke <= 0 || inner))
                svg_pixel(s, x, y, fill);
        }
}

static int svg_points(const char *text, int32_t *x, int32_t *y, int max)
{
    int n = 0;

    if (!text)
        return 0;
    while (*text && n < max) {
        const char *before = text;

        if (!svg_number(&text, &x[n]) ||
            !svg_number(&text, &y[n]))
            break;
        n++;
        if (text == before)
            break;
    }
    return n;
}

static void svg_fill_subpaths(struct svg_painter *s,
                              const int32_t *px, const int32_t *py,
                              const struct svg_subpath *subs, int nsub,
                              uint32_t color)
{
    int32_t ymin = 0, ymax = 0;
    int64_t y, yend;
    int sp, i, have = 0;

    if (!color || nsub <= 0)
        return;
    for (sp = 0; sp < nsub; sp++) {
        if (subs[sp].count < 3)
            continue;
        for (i = 0; i < subs[sp].count; i++) {
            int32_t vy = py[subs[sp].start + i];
            if (!have || vy < ymin) ymin = vy;
            if (!have || vy > ymax) ymax = vy;
            have = 1;
        }
    }
    if (!have)
        return;
    y = svg_max64(ymin, s->clip.y);
    yend = svg_min64(ymax,
                     (int64_t)s->clip.y + s->clip.h - 1);
    for (; y <= yend && s->work < SVG_WORK_MAX; y++) {
        int32_t cross[SVG_POINT_MAX];
        int nc = 0, a, b;

        for (sp = 0; sp < nsub && s->work < SVG_WORK_MAX; sp++) {
            int base = subs[sp].start;
            int cnt = subs[sp].count;

            if (cnt < 3)
                continue;
            for (i = 0; i < cnt && s->work < SVG_WORK_MAX; i++) {
                int j = i ? i - 1 : cnt - 1;
                int32_t y0 = py[base + j], y1 = py[base + i];

                s->work++;
                if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                    int64_t x = px[base + j] +
                        (y - (int64_t)y0) *
                        ((int64_t)px[base + i] - px[base + j]) /
                        ((int64_t)y1 - y0);

                    if (nc < SVG_POINT_MAX)
                        cross[nc++] = (int32_t)x;
                }
            }
        }
        for (a = 1; a < nc && s->work < SVG_WORK_MAX; a++) {
            int32_t v = cross[a];
            b = a;
            while (b > 0 && cross[b - 1] > v) {
                if (s->work++ >= SVG_WORK_MAX)
                    break;
                cross[b] = cross[b - 1];
                b--;
            }
            cross[b] = v;
        }
        for (a = 0; a + 1 < nc; a += 2) {
            int64_t x0 = svg_max64(cross[a], s->clip.x);
            int64_t x1 = svg_min64(
                cross[a + 1],
                (int64_t)s->clip.x + s->clip.w - 1);
            int64_t x;

            for (x = x0; x <= x1 && s->work < SVG_WORK_MAX; x++)
                svg_pixel(s, x, y, color);
        }
    }
}

static void svg_stroke_polyline(struct svg_painter *s,
                                const int32_t *x, const int32_t *y,
                                int count, int close, int32_t width,
                                uint32_t color)
{
    int i;

    if (!color || count < 2 || width <= 0)
        return;
    for (i = 1; i < count; i++)
        svg_line(s, x[i - 1], y[i - 1], x[i], y[i], width, color);
    if (close)
        svg_line(s, x[count - 1], y[count - 1], x[0], y[0], width, color);
}

/* Flatten SVG path commands into bounded subpaths. Supports multiple M/m..Z/z
 * subpaths, L/l, H/h, V/v, Q/q, T/t, C/c, S/s, and endpoint-only A/a. */
static int svg_path_subpaths(const char *d, int32_t *x, int32_t *y, int max,
                             struct svg_subpath *subs, int max_subs,
                             int *out_npts)
{
    int32_t cx = 0, cy = 0, sx = 0, sy = 0;
    int32_t last_cx2 = 0, last_cy2 = 0, last_qx = 0, last_qy = 0;
    char cmd = 0, prev_cmd = 0;
    int n = 0, nsub = 0, cur = -1;

    while (d && *d && n < max) {
        int relative;

        while (*d == ' ' || *d == '\t' || *d == '\r' || *d == '\n' ||
               *d == ',')
            d++;
        if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z'))
            cmd = *d++;
        if (!cmd)
            break;
        relative = cmd >= 'a' && cmd <= 'z';
        if (cmd == 'Z' || cmd == 'z') {
            if (cur >= 0) {
                subs[cur].closed = 1;
                cx = sx;
                cy = sy;
            }
            prev_cmd = cmd;
            cmd = 0;
            continue;
        }
        if (cmd == 'M' || cmd == 'm') {
            int32_t nx, ny;

            if (!svg_number(&d, &nx) || !svg_number(&d, &ny))
                break;
            if (relative) {
                nx = svg_sat_add(cx, nx);
                ny = svg_sat_add(cy, ny);
            }
            if (nsub >= max_subs)
                break;
            cur = nsub++;
            subs[cur].start = n;
            subs[cur].count = 0;
            subs[cur].closed = 0;
            cx = sx = nx;
            cy = sy = ny;
            x[n] = cx;
            y[n] = cy;
            n++;
            subs[cur].count++;
            prev_cmd = cmd;
            cmd = relative ? 'l' : 'L';
            continue;
        }
        if (cur < 0)
            break;
        if (cmd == 'L' || cmd == 'l') {
            int32_t nx, ny;

            if (!svg_number(&d, &nx) || !svg_number(&d, &ny))
                break;
            if (relative) {
                nx = svg_sat_add(cx, nx);
                ny = svg_sat_add(cy, ny);
            }
            cx = nx; cy = ny;
            x[n] = cx; y[n] = cy; n++;
            subs[cur].count++;
            prev_cmd = cmd;
        } else if (cmd == 'H' || cmd == 'h') {
            int32_t nx;
            if (!svg_number(&d, &nx)) break;
            cx = relative ? svg_sat_add(cx, nx) : nx;
            x[n] = cx; y[n] = cy; n++;
            subs[cur].count++;
            prev_cmd = cmd;
        } else if (cmd == 'V' || cmd == 'v') {
            int32_t ny;
            if (!svg_number(&d, &ny)) break;
            cy = relative ? svg_sat_add(cy, ny) : ny;
            x[n] = cx; y[n] = cy; n++;
            subs[cur].count++;
            prev_cmd = cmd;
        } else if (cmd == 'Q' || cmd == 'q' || cmd == 'T' || cmd == 't') {
            int32_t qx, qy, nx, ny, ox = cx, oy = cy;
            int step;

            if (cmd == 'Q' || cmd == 'q') {
                if (!svg_number(&d, &qx) || !svg_number(&d, &qy) ||
                    !svg_number(&d, &nx) || !svg_number(&d, &ny))
                    break;
                if (relative) {
                    qx = svg_sat_add(ox, qx);
                    qy = svg_sat_add(oy, qy);
                    nx = svg_sat_add(ox, nx);
                    ny = svg_sat_add(oy, ny);
                }
            } else {
                if (!svg_number(&d, &nx) || !svg_number(&d, &ny))
                    break;
                if (relative) {
                    nx = svg_sat_add(ox, nx);
                    ny = svg_sat_add(oy, ny);
                }
                if (prev_cmd == 'Q' || prev_cmd == 'q' ||
                    prev_cmd == 'T' || prev_cmd == 't') {
                    qx = svg_sat_coord(2LL * ox - last_qx);
                    qy = svg_sat_coord(2LL * oy - last_qy);
                } else {
                    qx = ox;
                    qy = oy;
                }
            }
            for (step = 1; step <= 12 && n < max; step++) {
                int64_t a = 12 - step, b = step;
                x[n] = (int32_t)((a * a * ox + 2 * a * b * qx +
                                  b * b * nx) / 144);
                y[n] = (int32_t)((a * a * oy + 2 * a * b * qy +
                                  b * b * ny) / 144);
                n++;
                subs[cur].count++;
            }
            last_qx = qx; last_qy = qy;
            cx = nx; cy = ny;
            prev_cmd = cmd;
        } else if (cmd == 'C' || cmd == 'c' || cmd == 'S' || cmd == 's') {
            int32_t x1, y1, x2, y2, nx, ny, ox = cx, oy = cy;
            int step;

            if (cmd == 'C' || cmd == 'c') {
                if (!svg_number(&d, &x1) || !svg_number(&d, &y1) ||
                    !svg_number(&d, &x2) || !svg_number(&d, &y2) ||
                    !svg_number(&d, &nx) || !svg_number(&d, &ny))
                    break;
                if (relative) {
                    x1 = svg_sat_add(ox, x1);
                    y1 = svg_sat_add(oy, y1);
                    x2 = svg_sat_add(ox, x2);
                    y2 = svg_sat_add(oy, y2);
                    nx = svg_sat_add(ox, nx);
                    ny = svg_sat_add(oy, ny);
                }
            } else {
                if (!svg_number(&d, &x2) || !svg_number(&d, &y2) ||
                    !svg_number(&d, &nx) || !svg_number(&d, &ny))
                    break;
                if (relative) {
                    x2 = svg_sat_add(ox, x2);
                    y2 = svg_sat_add(oy, y2);
                    nx = svg_sat_add(ox, nx);
                    ny = svg_sat_add(oy, ny);
                }
                if (prev_cmd == 'C' || prev_cmd == 'c' ||
                    prev_cmd == 'S' || prev_cmd == 's') {
                    x1 = svg_sat_coord(2LL * ox - last_cx2);
                    y1 = svg_sat_coord(2LL * oy - last_cy2);
                } else {
                    x1 = ox;
                    y1 = oy;
                }
            }
            for (step = 1; step <= 12 && n < max; step++) {
                int64_t a = 12 - step, b = step;
                x[n] = (int32_t)((a * a * a * ox +
                                  3 * a * a * b * x1 +
                                  3 * a * b * b * x2 +
                                  b * b * b * nx) / 1728);
                y[n] = (int32_t)((a * a * a * oy +
                                  3 * a * a * b * y1 +
                                  3 * a * b * b * y2 +
                                  b * b * b * ny) / 1728);
                n++;
                subs[cur].count++;
            }
            last_cx2 = x2; last_cy2 = y2;
            cx = nx; cy = ny;
            prev_cmd = cmd;
        } else if (cmd == 'A' || cmd == 'a') {
            int32_t discard, nx, ny;
            int k;

            for (k = 0; k < 5; k++)
                if (!svg_number(&d, &discard)) {
                    *out_npts = n;
                    return nsub;
                }
            if (!svg_number(&d, &nx) || !svg_number(&d, &ny)) {
                *out_npts = n;
                return nsub;
            }
            if (relative) {
                nx = svg_sat_add(cx, nx);
                ny = svg_sat_add(cy, ny);
            }
            cx = nx; cy = ny;
            x[n] = cx; y[n] = cy; n++;
            subs[cur].count++;
            prev_cmd = cmd;
        } else {
            break;
        }
    }
    *out_npts = n;
    return nsub;
}

static int svg_paint_points_node(struct svg_painter *s,
                                 const struct dom_node *n,
                                 const struct svg_style *st,
                                 int32_t sw, int path)
{
    int32_t x[SVG_POINT_MAX], y[SVG_POINT_MAX];
    struct svg_subpath subs[SVG_SUBPATH_MAX];
    int nsub, npts = 0, i, sp, any = 0;
    const char *source = dom_get_attr(n, path ? "d" : "points");

    if (path) {
        nsub = svg_path_subpaths(source, x, y, SVG_POINT_MAX,
                                 subs, SVG_SUBPATH_MAX, &npts);
    } else {
        npts = svg_points(source, x, y, SVG_POINT_MAX);
        subs[0].start = 0;
        subs[0].count = npts;
        subs[0].closed = (uint8_t)(n->tag && svg_ieq(n->tag, "polygon"));
        nsub = npts >= 2 ? 1 : 0;
    }
    if (nsub <= 0 || npts < 2)
        return 0;
    for (i = 0; i < npts; i++) {
        x[i] = svg_x(s, x[i]);
        y[i] = svg_y(s, y[i]);
    }
    if (st->fill)
        svg_fill_subpaths(s, x, y, subs, nsub, st->fill);
    for (sp = 0; sp < nsub; sp++) {
        if (subs[sp].count >= 2) {
            any = 1;
            svg_stroke_polyline(s, x + subs[sp].start, y + subs[sp].start,
                                subs[sp].count, subs[sp].closed, sw,
                                st->stroke);
        }
    }
    return any;
}

static void svg_style_for(const struct svg_painter *s,
                          const struct dom_node *n,
                          const struct svg_style *parent,
                          struct svg_style *out)
{
    const char *v;
    char value[80];

    *out = *parent;
    if (n->style && n != s->root &&
        (!s->root->style ||
         ((const struct computed_style *)n->style)->color !=
         ((const struct computed_style *)s->root->style)->color))
        out->current_color = 0xFF000000u |
            CSS_COLOR_RGB(((const struct computed_style *)n->style)->color);
    v = dom_get_attr(n, "color");
    if (v) out->current_color = svg_color(v, out->current_color,
                                          out->current_color);
    if (svg_style_value(n, "color", value, sizeof(value)))
        out->current_color = svg_color(value, out->current_color,
                                       out->current_color);

    v = dom_get_attr(n, "fill");
    if (v) out->fill = svg_color(v, out->fill, out->current_color);
    v = dom_get_attr(n, "stroke");
    if (v) out->stroke = svg_color(v, out->stroke, out->current_color);
    out->stroke_width =
        svg_attr_num(n, "stroke-width", out->stroke_width);
    if (svg_style_value(n, "fill", value, sizeof(value)))
        out->fill = svg_color(value, out->fill, out->current_color);
    if (svg_style_value(n, "stroke", value, sizeof(value)))
        out->stroke = svg_color(value, out->stroke, out->current_color);
    if (svg_style_value(n, "stroke-width", value, sizeof(value))) {
        const char *p = value;
        int32_t width;

        if (svg_number(&p, &width))
            out->stroke_width = width;
    }
    if (out->stroke_width < 0)
        out->stroke_width = 0;
}

static void svg_walk(struct svg_painter *s, const struct dom_node *n,
                     const struct svg_style *in, int depth)
{
    struct svg_style st;
    const struct dom_node *ch;
    const char *tag;
    int32_t sw;

    if (!n || depth > 16 || s->shapes >= SVG_SHAPE_MAX ||
        s->pixels >= SVG_PIXEL_MAX || s->work >= SVG_WORK_MAX)
        return;
    if (n->type != DOM_ELEMENT) {
        return;
    }
    svg_style_for(s, n, in, &st);
    tag = n->tag ? n->tag : "";
    sw = svg_w(s, st.stroke_width);
    if (sw < 1 && st.stroke && st.stroke_width > 0) sw = 1;

    if (svg_ieq(tag, "rect")) {
        int32_t x = svg_x(s, svg_attr_num(n, "x", 0));
        int32_t y = svg_y(s, svg_attr_num(n, "y", 0));
        int32_t w = svg_w(s, svg_attr_num(n, "width", 0));
        int32_t h = svg_h(s, svg_attr_num(n, "height", 0));

        if (w > 0 && h > 0) {
            if (st.fill)
                svg_fill_rect(s, lay_mkrect(x, y, w, h), st.fill);
            if (st.stroke && sw > 0) {
                svg_fill_rect(s, lay_mkrect(x, y, w, sw), st.stroke);
                svg_fill_rect(s, lay_mkrect(x, y + h - sw, w, sw),
                              st.stroke);
                svg_fill_rect(s, lay_mkrect(x, y, sw, h), st.stroke);
                svg_fill_rect(s, lay_mkrect(x + w - sw, y, sw, h),
                              st.stroke);
            }
            s->shapes++;
        }
    } else if (svg_ieq(tag, "circle") || svg_ieq(tag, "ellipse")) {
        int32_t rx, ry;
        int32_t cx = svg_x(s, svg_attr_num(n, "cx", 0));
        int32_t cy = svg_y(s, svg_attr_num(n, "cy", 0));

        if (svg_ieq(tag, "circle")) {
            int32_t r = svg_attr_num(n, "r", 0);
            rx = svg_w(s, r);
            ry = svg_h(s, r);
        } else {
            rx = svg_w(s, svg_attr_num(n, "rx", 0));
            ry = svg_h(s, svg_attr_num(n, "ry", 0));
        }
        svg_ellipse(s, cx, cy, rx, ry, sw, st.fill,
                    sw > 0 ? st.stroke : 0);
        s->shapes++;
    } else if (svg_ieq(tag, "line")) {
        svg_line(s,
                 svg_x(s, svg_attr_num(n, "x1", 0)),
                 svg_y(s, svg_attr_num(n, "y1", 0)),
                 svg_x(s, svg_attr_num(n, "x2", 0)),
                 svg_y(s, svg_attr_num(n, "y2", 0)),
                 sw, st.stroke);
        s->shapes++;
    } else if (svg_ieq(tag, "polyline") ||
               svg_ieq(tag, "polygon")) {
        if (svg_paint_points_node(s, n, &st, sw, 0))
            s->shapes++;
    } else if (svg_ieq(tag, "path")) {
        if (svg_paint_points_node(s, n, &st, sw, 1))
            s->shapes++;
    } else if (svg_ieq(tag, "text")) {
        const struct dom_node *text = n->first_child;

        while (text && text->type != DOM_TEXT)
            text = text->next_sibling;
        if (text && text->text_len && st.fill) {
            unsigned long chars = text->text_len;
            const struct font *f = font_get(FONT_BODY, FONT_REGULAR,
                                            FONT_ROMAN);
            font_surface fs =
                font_surface_of(s->t->px, s->t->w, s->t->h, tstride(s->t));
            font_rc rc = font_mkrc(s->clip.x, s->clip.y,
                                   s->clip.w, s->clip.h);
            int32_t x = svg_x(s, svg_attr_num(n, "x", 0));
            int32_t y = svg_y(s, svg_attr_num(n, "y", 0)) -
                        font_ascent(f);

            if (chars > (SVG_WORK_MAX - s->work) / 128)
                chars = (SVG_WORK_MAX - s->work) / 128;
            if (chars > 32768)
                chars = 32768;
            s->work += chars * 128;
            font_draw_text_n(&fs, rc, x, y, text->text,
                             (int)chars, f,
                             st.fill & 0xFFFFFFu, FONT_TRANSPARENT);
            if (chars)
                s->shapes++;
        }
    }
    for (ch = n->first_child; ch; ch = ch->next_sibling)
        if (ch->type == DOM_ELEMENT)
            svg_walk(s, ch, &st, depth + 1);
}

static void svg_parse_preserve(const char *preserve, int *use_preserve,
                               int *xalign, int *yalign, int *slice)
{
    const char *p = preserve;
    const char *tok;
    unsigned long len;

    *use_preserve = 1;
    *xalign = 1;
    *yalign = 1;
    *slice = 0;
    if (!p)
        return;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    len = (unsigned long)(p - tok);
    if (svg_ieq_n(tok, len, "none")) {
        *use_preserve = 0;
        return;
    }
    if (len == 8) {
        if (svg_ieq_n(tok, 4, "xMin")) *xalign = 0;
        else if (svg_ieq_n(tok, 4, "xMid")) *xalign = 1;
        else if (svg_ieq_n(tok, 4, "xMax")) *xalign = 2;
        if (svg_ieq_n(tok + 4, 4, "YMin")) *yalign = 0;
        else if (svg_ieq_n(tok + 4, 4, "YMid")) *yalign = 1;
        else if (svg_ieq_n(tok + 4, 4, "YMax")) *yalign = 2;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    tok = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    len = (unsigned long)(p - tok);
    if (svg_ieq_n(tok, len, "slice"))
        *slice = 1;
}

static int paint_inline_svg(const struct paint_target *t, lay_rect clip,
                            lay_rect dst, const struct dom_node *root,
                            uint32_t current_color, unsigned opacity)
{
    struct svg_painter s;
    struct svg_style st;
    const char *view, *preserve;
    int32_t v[4];
    int i, use_preserve, xalign, yalign, slice;

    if (!root || root->tag_id != HTAG_SVG || dst.w <= 0 || dst.h <= 0)
        return 0;
    memset(&s, 0, sizeof(s));
    s.t = t;
    s.clip = lay_rect_intersect(clip, dst);
    s.dst = dst;
    s.minx = s.miny = 0;
    s.vieww = (int64_t)dst.w * 1000;
    s.viewh = (int64_t)dst.h * 1000;
    s.root = root;
    s.current_color = 0xFF000000u | CSS_COLOR_RGB(current_color);
    s.opacity = opacity > 1000 ? 1000 : opacity;
    view = dom_get_attr(root, "viewbox");
    if (view) {
        const char *p = view;

        for (i = 0; i < 4; i++)
            if (!svg_number(&p, &v[i]))
                break;
        if (i == 4 && v[2] > 0 && v[3] > 0) {
            s.minx = v[0]; s.miny = v[1];
            s.vieww = v[2]; s.viewh = v[3];
        }
    }
    preserve = dom_get_attr(root, "preserveaspectratio");
    svg_parse_preserve(preserve, &use_preserve, &xalign, &yalign, &slice);
    if (view && use_preserve) {
        int fit_by_width = !slice
            ? ((int64_t)dst.w * s.viewh <= (int64_t)dst.h * s.vieww)
            : ((int64_t)dst.w * s.viewh >= (int64_t)dst.h * s.vieww);

        if (fit_by_width) {
            int64_t by_width = s.viewh * dst.w / s.vieww;
            int64_t diff;

            if (by_width < 1) by_width = 1;
            diff = dst.h - by_width;
            s.dst.y = svg_sat_coord((int64_t)dst.y +
                                    (yalign == 0 ? 0 :
                                     (yalign == 2 ? diff : diff / 2)));
            s.dst.h = (int32_t)by_width;
        } else {
            int64_t by_height = s.vieww * dst.h / s.viewh;
            int64_t diff;

            if (by_height < 1) by_height = 1;
            diff = dst.w - by_height;
            s.dst.x = svg_sat_coord((int64_t)dst.x +
                                    (xalign == 0 ? 0 :
                                     (xalign == 2 ? diff : diff / 2)));
            s.dst.w = (int32_t)by_height;
        }
    }
    st.fill = 0xFF000000u;
    st.stroke = 0;
    st.current_color = s.current_color;
    st.stroke_width = 1000;
    svg_walk(&s, root, &st, 0);
    return s.shapes > 0;
}

/* A horizontal or vertical run drawn in a border style. Dashes and dots
 * are approximated with a fixed period derived from the border width,
 * which is what the specification leaves up to the implementation. */
static void stroke_run(const struct paint_target *t, lay_rect clip,
                       lay_rect r, uint32_t color, int style, int horiz,
                       int32_t width)
{
    int32_t period, on, i;

    switch (style) {
    case CSS_BORDERSTYLE_NONE:
    case CSS_BORDERSTYLE_HIDDEN:
        return;
    case CSS_BORDERSTYLE_DOUBLE: {
        int32_t third = width / 3;

        if (third < 1) third = 1;
        if (horiz) {
            paint_fill(t, clip, lay_mkrect(r.x, r.y, r.w, third), color);
            paint_fill(t, clip, lay_mkrect(r.x, r.y + r.h - third, r.w, third),
                       color);
        } else {
            paint_fill(t, clip, lay_mkrect(r.x, r.y, third, r.h), color);
            paint_fill(t, clip, lay_mkrect(r.x + r.w - third, r.y, third, r.h),
                       color);
        }
        return;
    }
    case CSS_BORDERSTYLE_DASHED:
        period = width * 3 > 3 ? width * 3 : 3;
        on = period * 2 / 3;
        break;
    case CSS_BORDERSTYLE_DOTTED:
        period = width * 2 > 2 ? width * 2 : 2;
        on = width > 0 ? width : 1;
        break;
    case CSS_BORDERSTYLE_GROOVE:
    case CSS_BORDERSTYLE_RIDGE:
    case CSS_BORDERSTYLE_INSET:
    case CSS_BORDERSTYLE_OUTSET: {
        int dark = (style == CSS_BORDERSTYLE_GROOVE ||
                    style == CSS_BORDERSTYLE_INSET);
        uint32_t hi = 0xFF000000u | shade(color, 35);
        uint32_t lo = 0xFF000000u | shade(color, -35);
        int32_t half = width / 2;

        if (half < 1) half = 1;
        if (style == CSS_BORDERSTYLE_INSET || style == CSS_BORDERSTYLE_OUTSET) {
            paint_fill(t, clip, r, dark ? lo : hi);
            return;
        }
        if (horiz) {
            paint_fill(t, clip, lay_mkrect(r.x, r.y, r.w, half),
                       dark ? lo : hi);
            paint_fill(t, clip, lay_mkrect(r.x, r.y + half, r.w, r.h - half),
                       dark ? hi : lo);
        } else {
            paint_fill(t, clip, lay_mkrect(r.x, r.y, half, r.h),
                       dark ? lo : hi);
            paint_fill(t, clip, lay_mkrect(r.x + half, r.y, r.w - half, r.h),
                       dark ? hi : lo);
        }
        return;
    }
    case CSS_BORDERSTYLE_SOLID:
    default:
        paint_fill(t, clip, r, color);
        return;
    }
    if (horiz) {
        for (i = 0; i < r.w; i += period) {
            int32_t seg = pmin(on, r.w - i);

            paint_fill(t, clip, lay_mkrect(r.x + i, r.y, seg, r.h), color);
        }
    } else {
        for (i = 0; i < r.h; i += period) {
            int32_t seg = pmin(on, r.h - i);

            paint_fill(t, clip, lay_mkrect(r.x, r.y + i, r.w, seg), color);
        }
    }
}

void paint_frame(const struct paint_target *t, lay_rect clip, lay_rect r,
                 int32_t top, int32_t right, int32_t bottom, int32_t left,
                 const uint32_t *colors, const uint8_t *styles)
{
    if (top > 0)
        stroke_run(t, clip, lay_mkrect(r.x, r.y, r.w, top),
                   colors[CSS_TOP], styles[CSS_TOP], 1, top);
    if (bottom > 0)
        stroke_run(t, clip, lay_mkrect(r.x, r.y + r.h - bottom, r.w, bottom),
                   colors[CSS_BOTTOM], styles[CSS_BOTTOM], 1, bottom);
    if (left > 0)
        stroke_run(t, clip, lay_mkrect(r.x, r.y + top, left,
                                       r.h - top - bottom),
                   colors[CSS_LEFT], styles[CSS_LEFT], 0, left);
    if (right > 0)
        stroke_run(t, clip, lay_mkrect(r.x + r.w - right, r.y + top, right,
                                       r.h - top - bottom),
                   colors[CSS_RIGHT], styles[CSS_RIGHT], 0, right);
}

/* ------------------------------------------------------------- helpers */

static uint32_t isqrt64(uint64_t n)
{
    uint64_t x = 0, bit = 1ULL << 62;

    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= x + bit) {
            n -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)x;
}

static int has_any_radius(const int32_t r[4])
{
    return r[0] > 0 || r[1] > 0 || r[2] > 0 || r[3] > 0;
}

static void clamp_radii(lay_rect r, const int32_t raw[4], int32_t out[4])
{
    int i;
    int64_t num = 1000000LL, den = 1000000LL;
    int32_t sums[4], lims[4];

    for (i = 0; i < 4; i++)
        out[i] = raw[i] > 0 ? raw[i] : 0;
    if (r.w <= 0 || r.h <= 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    sums[0] = out[CSS_TL] + out[CSS_TR]; lims[0] = r.w;
    sums[1] = out[CSS_BL] + out[CSS_BR]; lims[1] = r.w;
    sums[2] = out[CSS_TL] + out[CSS_BL]; lims[2] = r.h;
    sums[3] = out[CSS_TR] + out[CSS_BR]; lims[3] = r.h;
    for (i = 0; i < 4; i++) {
        if (sums[i] > lims[i] && sums[i] > 0) {
            if ((int64_t)lims[i] * den < num * sums[i]) {
                num = lims[i];
                den = sums[i];
            }
        }
    }
    if (num < den) {
        for (i = 0; i < 4; i++)
            out[i] = (int32_t)((int64_t)out[i] * num / den);
    }
}

/* Signed distance (in 1/256 px) from pixel center (x+0.5, y+0.5) to
 * rounded rectangle `r` with elliptical corner radii (rx[4], ry[4]).
 * Negative = inside, 0 = on boundary, positive = outside. */
static int64_t rrect_sd256(int32_t x, int32_t y, lay_rect r,
                           const int32_t rx[4], const int32_t ry[4])
{
    int64_t px2 = (int64_t)x * 2 + 1;
    int64_t py2 = (int64_t)y * 2 + 1;
    int64_t x0_2 = (int64_t)r.x * 2;
    int64_t x1_2 = (int64_t)(r.x + r.w) * 2;
    int64_t y0_2 = (int64_t)r.y * 2;
    int64_t y1_2 = (int64_t)(r.y + r.h) * 2;
    int64_t dx = 0, dy = 0;
    int32_t a = 0, b = 0;

    if (r.w <= 0 || r.h <= 0)
        return 1000000LL;
    if (rx[CSS_TL] > 0 && ry[CSS_TL] > 0 &&
        px2 < x0_2 + 2LL * rx[CSS_TL] && py2 < y0_2 + 2LL * ry[CSS_TL]) {
        dx = (x0_2 + 2LL * rx[CSS_TL]) - px2;
        dy = (y0_2 + 2LL * ry[CSS_TL]) - py2;
        a = rx[CSS_TL]; b = ry[CSS_TL];
    } else if (rx[CSS_TR] > 0 && ry[CSS_TR] > 0 &&
               px2 > x1_2 - 2LL * rx[CSS_TR] && py2 < y0_2 + 2LL * ry[CSS_TR]) {
        dx = px2 - (x1_2 - 2LL * rx[CSS_TR]);
        dy = (y0_2 + 2LL * ry[CSS_TR]) - py2;
        a = rx[CSS_TR]; b = ry[CSS_TR];
    } else if (rx[CSS_BR] > 0 && ry[CSS_BR] > 0 &&
               px2 > x1_2 - 2LL * rx[CSS_BR] && py2 > y1_2 - 2LL * ry[CSS_BR]) {
        dx = px2 - (x1_2 - 2LL * rx[CSS_BR]);
        dy = py2 - (y1_2 - 2LL * ry[CSS_BR]);
        a = rx[CSS_BR]; b = ry[CSS_BR];
    } else if (rx[CSS_BL] > 0 && ry[CSS_BL] > 0 &&
               px2 < x0_2 + 2LL * rx[CSS_BL] && py2 > y1_2 - 2LL * ry[CSS_BL]) {
        dx = (x0_2 + 2LL * rx[CSS_BL]) - px2;
        dy = py2 - (y1_2 - 2LL * ry[CSS_BL]);
        a = rx[CSS_BL]; b = ry[CSS_BL];
    } else {
        int64_t dx_out = x0_2 - px2;
        int64_t dr = px2 - x1_2;
        int64_t dy_out = y0_2 - py2;
        int64_t db = py2 - y1_2;

        if (dr > dx_out) dx_out = dr;
        if (db > dy_out) dy_out = db;
        if (dx_out > 0 && dy_out > 0) {
            if (dx_out > 100000LL) dx_out = 100000LL;
            if (dy_out > 100000LL) dy_out = 100000LL;
            return (int64_t)isqrt64(
                ((uint64_t)dx_out * dx_out + (uint64_t)dy_out * dy_out) *
                16384ULL);
        }
        return (dx_out > dy_out ? dx_out : dy_out) * 128LL;
    }

    if (dx > 100000LL || dy > 100000LL)
        return 10000000LL;
    if (a == b) {
        int64_t d256 = (int64_t)isqrt64(
            ((uint64_t)dx * dx + (uint64_t)dy * dy) * 16384ULL);
        return d256 - (int64_t)a * 256LL;
    }
    {
        int64_t u = dx * 2048LL / a;
        int64_t v = dy * 2048LL / b;
        int64_t rn = (int64_t)isqrt64((uint64_t)u * u + (uint64_t)v * v);
        int32_t m = pmin(a, b);
        int64_t gx = u * m / a;
        int64_t gy = v * m / b;
        int64_t g = (int64_t)isqrt64((uint64_t)gx * gx + (uint64_t)gy * gy);

        return g > 0 ? ((rn * (rn - 4096LL) / 4096LL) * 256LL * m / g) : 0;
    }
}

static unsigned sd_to_cov(int64_t sd_256)
{
    int64_t c;

    if (sd_256 <= -128) return 255;
    if (sd_256 >= 128)  return 0;
    c = (255 * (128 - sd_256) + 128) / 256;
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    return (unsigned)c;
}

#define RCLIP_MAX 8

struct rclip_entry {
    lay_rect rect;
    int32_t rx[4], ry[4];
};

struct rclip_list {
    struct rclip_entry e[RCLIP_MAX];
    int n;
};

static unsigned rclip_cov(const struct rclip_list *rc, int32_t x, int32_t y)
{
    unsigned cov = 255;
    int i;

    for (i = 0; i < rc->n; i++) {
        unsigned c = sd_to_cov(rrect_sd256(x, y, rc->e[i].rect,
                                           rc->e[i].rx, rc->e[i].ry));
        if (c < cov)
            cov = c;
        if (!cov)
            return 0;
    }
    return cov;
}

static unsigned scale_alpha(unsigned a, unsigned opacity)
{
    if (opacity >= 1000)
        return a;
    return (unsigned)((uint64_t)a * opacity / 1000);
}

static void box_inner_radii(const struct lay_box *b, lay_rect br,
                            int32_t rx_in[4], int32_t ry_in[4])
{
    int32_t ro[4];

    clamp_radii(br, b->style->border_radius, ro);
    rx_in[CSS_TL] = pmax(0, ro[CSS_TL] - b->bord[CSS_LEFT]);
    ry_in[CSS_TL] = pmax(0, ro[CSS_TL] - b->bord[CSS_TOP]);
    rx_in[CSS_TR] = pmax(0, ro[CSS_TR] - b->bord[CSS_RIGHT]);
    ry_in[CSS_TR] = pmax(0, ro[CSS_TR] - b->bord[CSS_TOP]);
    rx_in[CSS_BR] = pmax(0, ro[CSS_BR] - b->bord[CSS_RIGHT]);
    ry_in[CSS_BR] = pmax(0, ro[CSS_BR] - b->bord[CSS_BOTTOM]);
    rx_in[CSS_BL] = pmax(0, ro[CSS_BL] - b->bord[CSS_LEFT]);
    ry_in[CSS_BL] = pmax(0, ro[CSS_BL] - b->bord[CSS_BOTTOM]);
}

/* Fixed-point sin(deg) * 16384 for 0..90 degrees. */
static const int16_t grad_sin90[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406,
    3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402, 6664,
    6924, 7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923, 9162, 9397, 9630,
    9860, 10087, 10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786,
    11982, 12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421, 13583,
    13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726, 14849, 14968,
    15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897,
    15964, 16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344,
    16362, 16374, 16382, 16384
};

static void grad_dir_vec(int deg, int32_t *vx, int32_t *vy)
{
    int a = ((deg % 360) + 360) % 360;

    if (a <= 90) {
        *vx = grad_sin90[a];
        *vy = -grad_sin90[90 - a];
    } else if (a <= 180) {
        *vx = grad_sin90[180 - a];
        *vy = grad_sin90[a - 90];
    } else if (a <= 270) {
        *vx = -grad_sin90[a - 180];
        *vy = grad_sin90[270 - a];
    } else {
        *vx = -grad_sin90[360 - a];
        *vy = -grad_sin90[a - 270];
    }
}

static uint32_t grad_sample(const struct computed_style *cs, int32_t t)
{
    int n = cs->grad_nstops;
    int i;

    if (n <= 0)
        return 0;
    if (n > CSS_GRAD_MAX_STOPS)
        n = CSS_GRAD_MAX_STOPS;
    if (n == 1 || t <= cs->grad_pos[0])
        return cs->grad_colors[0];
    if (t >= cs->grad_pos[n - 1])
        return cs->grad_colors[n - 1];
    for (i = 0; i < n - 1; i++) {
        int32_t p0 = cs->grad_pos[i];
        int32_t p1 = cs->grad_pos[i + 1];

        if (t <= p1) {
            int32_t span = p1 - p0;
            uint32_t c0 = cs->grad_colors[i];
            uint32_t c1 = cs->grad_colors[i + 1];
            uint32_t ca, cr, cg, cb;
            int32_t num;

            if (span <= 0)
                return c1;
            num = t - p0;
            ca = (uint32_t)(CSS_COLOR_A(c0) +
                ((int32_t)CSS_COLOR_A(c1) - (int32_t)CSS_COLOR_A(c0)) * num / span);
            cr = (uint32_t)(CSS_COLOR_R(c0) +
                ((int32_t)CSS_COLOR_R(c1) - (int32_t)CSS_COLOR_R(c0)) * num / span);
            cg = (uint32_t)(CSS_COLOR_G(c0) +
                ((int32_t)CSS_COLOR_G(c1) - (int32_t)CSS_COLOR_G(c0)) * num / span);
            cb = (uint32_t)(CSS_COLOR_B(c0) +
                ((int32_t)CSS_COLOR_B(c1) - (int32_t)CSS_COLOR_B(c0)) * num / span);
            return (ca << 24) | (cr << 16) | (cg << 8) | cb;
        }
    }
    return cs->grad_colors[n - 1];
}

/* ------------------------------------------------------------- images */

static void blit_image_ex(const struct paint_target *t, lay_rect clip,
                          lay_rect dst, const struct image *im,
                          unsigned opacity, const struct rclip_list *rc)
{
    int32_t x0, y0, x1, y1, x, y, st;
    lay_rect r;

    if (!im || im->w <= 0 || im->h <= 0 || dst.w <= 0 || dst.h <= 0 || !opacity)
        return;
    r = lay_rect_intersect(dst, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    st = tstride(t);
    /* Nearest neighbour: the caller may pre-scale with img_scale() when
     * quality matters; this keeps the painter allocation-free. */
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;
        int32_t sy = (int32_t)(((long)(y - dst.y) * im->h) / dst.h);

        if (sy < 0) sy = 0;
        if (sy >= im->h) sy = im->h - 1;
        for (x = x0; x < x1; x++) {
            int32_t sx = (int32_t)(((long)(x - dst.x) * im->w) / dst.w);
            uint32_t px;
            long si;
            unsigned a;

            if (sx < 0) sx = 0;
            if (sx >= im->w) sx = im->w - 1;
            si = (long)sy * im->w + sx;
            px = im->px[si];
            a = (im->has_alpha && im->alpha) ? im->alpha[si] : 255u;
            if (opacity < 1000)
                a = scale_alpha(a, opacity);
            if (rc && rc->n > 0)
                a = a * rclip_cov(rc, x, y) / 255u;
            if (!a)
                continue;
            row[x] = a >= 255u ? (px & 0x00FFFFFFu) : paint_blend(row[x], px, a);
        }
    }
}

static void blit_image(const struct paint_target *t, lay_rect clip,
                       lay_rect dst, const struct image *im)
{
    blit_image_ex(t, clip, dst, im, 1000, 0);
}

/* ------------------------------------------------------- list markers */

static void draw_marker_glyph(const struct paint_target *t, lay_rect clip,
                              const struct lay_box *b, int32_t dx, int32_t dy,
                              uint32_t color)
{
    const struct font *f = b->font ? b->font : font_get(FONT_BODY, 0, 0);
    int32_t cx, cy, rad;

    rad = f->w / 4;
    if (rad < 2)
        rad = 2;
    cx = b->x + dx + f->w / 2;
    cy = b->y + dy + font_ascent(f) - f->xheight / 2;

    switch (b->style->list_style_type) {
    case CSS_LISTSTYLE_SQUARE:
        paint_fill(t, clip, lay_mkrect(cx - rad, cy - rad, rad * 2, rad * 2),
                   color);
        return;
    case CSS_LISTSTYLE_CIRCLE: {
        int32_t i, j;

        for (j = -rad; j <= rad; j++)
            for (i = -rad; i <= rad; i++) {
                int32_t d = i * i + j * j;

                if (d <= rad * rad && d >= (rad - 1) * (rad - 1))
                    paint_fill(t, clip, lay_mkrect(cx + i, cy + j, 1, 1),
                               color);
            }
        return;
    }
    case CSS_LISTSTYLE_DISC:
    default: {
        int32_t i, j;

        for (j = -rad; j <= rad; j++)
            for (i = -rad; i <= rad; i++)
                if (i * i + j * j <= rad * rad)
                    paint_fill(t, clip, lay_mkrect(cx + i, cy + j, 1, 1),
                               color);
        return;
    }
    }
}

/* ---------------------------------------------------------------- text */

static void draw_decorations(const struct paint_target *t, lay_rect clip,
                             const struct lay_box *b, int32_t x, int32_t y,
                             int32_t w, uint32_t color)
{
    const struct font *f = b->font;
    unsigned d = b->style->text_decoration;
    int32_t thick = f->h >= 24 ? 2 : 1;

    if (d & CSS_DECOR_UNDERLINE)
        paint_fill(t, clip, lay_mkrect(x, y + font_ascent(f) + 1, w, thick),
                   color);
    if (d & CSS_DECOR_OVERLINE)
        paint_fill(t, clip, lay_mkrect(x, y, w, thick), color);
    if (d & CSS_DECOR_LINETHRU)
        paint_fill(t, clip,
                   lay_mkrect(x, y + font_ascent(f) - f->xheight / 2, w, thick),
                   color);
}

/* ------------------------------------------------------------ painting */

void paint_opts_init(struct paint_opts *o)
{
    memset(o, 0, sizeof *o);
    o->canvas = 0xFFFFFFFFu;
    o->caret_color = 0xFF000000u;
    o->highlight_color = 0xFF5BA3D0u;
}

struct pstate {
    const struct lay_document *L;
    const struct paint_target *t;
    const struct paint_opts *o;
    struct paint_stats *st;
    lay_rect dirty;
    int32_t dx, dy;      /* document -> surface translation */
};

static void box_shift(const struct pstate *p, const struct lay_box *b,
                      int32_t *out_dx, int32_t *out_dy)
{
    if (b->flags & (LAYF_FIXED | LAYF_FIXED_CTX)) {
        *out_dx = p->o->origin_x;
        *out_dy = p->o->origin_y;
        return;
    }
    if (b->flags & (LAYF_STICKY | LAYF_STICKY_CTX)) {
        int32_t sdx = 0, sdy = 0;

        lay_sticky_offset(p->L, b, p->o->scroll_x, p->o->scroll_y, &sdx, &sdy);
        *out_dx = p->dx + sdx;
        *out_dy = p->dy + sdy;
        return;
    }
    *out_dx = p->dx;
    *out_dy = p->dy;
}

static lay_rect shift_rect(lay_rect r, int32_t dx, int32_t dy)
{
    r.x += dx;
    r.y += dy;
    return r;
}

static lay_rect box_clip_rect(const struct pstate *p, const struct lay_box *b,
                              int32_t bx, int32_t by)
{
    if (b->flags & (LAYF_STICKY | LAYF_STICKY_CTX)) {
        const struct lay_box *s = b, *anc;
        lay_rect ac, oc;
        int guard = 0;

        while (s && !(s->flags & LAYF_STICKY) && guard++ < LAY_MAX_DEPTH * 2)
            s = s->parent;
        if (s) {
            ac = s->parent
                ? shift_rect(lay_clip_rect(s->parent), p->dx, p->dy)
                : lay_mkrect(-0x20000000, -0x20000000, 0x40000000, 0x40000000);
            oc = lay_mkrect(-0x20000000, -0x20000000, 0x40000000, 0x40000000);
            for (anc = b; anc && guard++ < LAY_MAX_DEPTH * 4; anc = anc->parent) {
                if (anc->flags & LAYF_CLIP)
                    oc = lay_rect_intersect(oc, lay_padding_rect(anc));
                if (anc == s)
                    break;
            }
            oc = shift_rect(oc, bx, by);
            return lay_rect_intersect(ac, oc);
        }
    }
    return shift_rect(lay_clip_rect(b), bx, by);
}

static lay_rect box_self_clip_rect(const struct pstate *p,
                                   const struct lay_box *b,
                                   int32_t bx, int32_t by)
{
    if (b->flags & LAYF_FIXED)
        return lay_mkrect(-0x20000000, -0x20000000, 0x40000000, 0x40000000);
    if (b->flags & LAYF_CLIP) {
        if (b->parent) {
            int32_t px, py;
            box_shift(p, b->parent, &px, &py);
            return box_clip_rect(p, b->parent, px, py);
        }
        return lay_mkrect(-0x20000000, -0x20000000, 0x40000000, 0x40000000);
    }
    return box_clip_rect(p, b, bx, by);
}

static unsigned effective_opacity(const struct lay_box *b)
{
    uint64_t op = 1000;
    int guard = 0;

    while (b && guard++ < LAY_MAX_DEPTH * 2) {
        if (!(b->flags & LAYF_ANON) && b->style && b->style->opacity < 1000)
            op = op * b->style->opacity / 1000;
        b = b->parent;
    }
    return (unsigned)op;
}

static void collect_rclips(const struct pstate *p, const struct lay_box *b,
                           int include_self, struct rclip_list *rc)
{
    const struct lay_box *a;
    int guard = 0;

    rc->n = 0;
    if (!include_self && (b->flags & LAYF_FIXED))
        return;
    a = include_self ? b : b->parent;
    for (; a && guard++ < LAY_MAX_DEPTH * 2 && rc->n < RCLIP_MAX; a = a->parent) {
        if ((a->flags & LAYF_CLIP) && a->style &&
            has_any_radius(a->style->border_radius)) {
            int32_t adx, ady;
            lay_rect pr, br;
            struct rclip_entry *e = &rc->e[rc->n++];

            box_shift(p, a, &adx, &ady);
            br = shift_rect(lay_border_rect(a), adx, ady);
            pr = shift_rect(lay_padding_rect(a), adx, ady);
            e->rect = pr;
            box_inner_radii(a, br, e->rx, e->ry);
        }
        if (a->flags & LAYF_FIXED)
            break;
    }
}

static int rclip_touches_corners(const struct rclip_list *rc, lay_rect r)
{
    int i;

    for (i = 0; i < rc->n; i++) {
        lay_rect e = rc->e[i].rect;
        int32_t rx, ry;

        rx = rc->e[i].rx[CSS_TL]; ry = rc->e[i].ry[CSS_TL];
        if (rx > 0 && ry > 0 &&
            lay_rect_intersects(r, lay_mkrect(e.x, e.y, rx, ry)))
            return 1;
        rx = rc->e[i].rx[CSS_TR]; ry = rc->e[i].ry[CSS_TR];
        if (rx > 0 && ry > 0 &&
            lay_rect_intersects(r, lay_mkrect(e.x + e.w - rx, e.y, rx, ry)))
            return 1;
        rx = rc->e[i].rx[CSS_BR]; ry = rc->e[i].ry[CSS_BR];
        if (rx > 0 && ry > 0 &&
            lay_rect_intersects(r, lay_mkrect(e.x + e.w - rx, e.y + e.h - ry,
                                              rx, ry)))
            return 1;
        rx = rc->e[i].rx[CSS_BL]; ry = rc->e[i].ry[CSS_BL];
        if (rx > 0 && ry > 0 &&
            lay_rect_intersects(r, lay_mkrect(e.x, e.y + e.h - ry, rx, ry)))
            return 1;
    }
    return 0;
}

static void paint_fill_ex(const struct paint_target *t, lay_rect clip,
                          lay_rect r, uint32_t color, unsigned opacity,
                          const struct rclip_list *rc)
{
    int32_t x0, y0, x1, y1, x, y, st;
    unsigned a = CSS_COLOR_A(color);

    if (!t || !t->px || !a || !opacity)
        return;
    if (opacity == 1000 && (!rc || rc->n == 0)) {
        paint_fill(t, clip, r, color);
        return;
    }
    r = lay_rect_intersect(r, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    a = scale_alpha(a, opacity);
    if (!a)
        return;
    st = tstride(t);
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;

        for (x = x0; x < x1; x++) {
            unsigned c = a;

            if (rc && rc->n > 0)
                c = c * rclip_cov(rc, x, y) / 255u;
            if (!c)
                continue;
            row[x] = c >= 255u ? (color & 0x00FFFFFFu)
                               : paint_blend(row[x], color, c);
        }
    }
}

static void paint_fill_rrect(const struct paint_target *t, lay_rect clip,
                             lay_rect shape, const int32_t rx[4],
                             const int32_t ry[4], uint32_t color,
                             unsigned opacity, const struct rclip_list *rc)
{
    int32_t x0, y0, x1, y1, x, y, st;
    unsigned a = scale_alpha(CSS_COLOR_A(color), opacity);
    lay_rect r;

    if (!t || !t->px || !a)
        return;
    r = lay_rect_intersect(shape, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    st = tstride(t);
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;

        for (x = x0; x < x1; x++) {
            unsigned cov = sd_to_cov(rrect_sd256(x, y, shape, rx, ry));
            unsigned ca;

            if (!cov)
                continue;
            ca = a * cov / 255u;
            if (rc && rc->n > 0)
                ca = ca * rclip_cov(rc, x, y) / 255u;
            if (!ca)
                continue;
            row[x] = ca >= 255u ? (color & 0x00FFFFFFu)
                                : paint_blend(row[x], color, ca);
        }
    }
}

static void paint_grad_box(const struct paint_target *t, lay_rect clip,
                           lay_rect grad_rect, lay_rect shape,
                           const int32_t rx[4], const int32_t ry[4],
                           int rounded, int opaque_bord, lay_rect br,
                           const int32_t r_out[4],
                           const struct computed_style *cs,
                           unsigned opacity, const struct rclip_list *rc)
{
    int32_t x0, y0, x1, y1, x, y, st;
    int32_t vx, vy, avx, avy;
    int64_t cx2, cy2, L_span;
    lay_rect r;

    if (!t || !t->px || !opacity || grad_rect.w <= 0 || grad_rect.h <= 0 ||
        !cs->grad_nstops)
        return;
    r = lay_rect_intersect(opaque_bord ? br : shape, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    grad_dir_vec(cs->grad_angle_deg, &vx, &vy);
    avx = vx < 0 ? -vx : vx;
    avy = vy < 0 ? -vy : vy;
    cx2 = (int64_t)grad_rect.x * 2 + grad_rect.w;
    cy2 = (int64_t)grad_rect.y * 2 + grad_rect.h;
    L_span = (int64_t)grad_rect.w * avx + (int64_t)grad_rect.h * avy;
    if (L_span <= 0)
        L_span = 1;
    st = tstride(t);
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;
        int64_t dy2 = (int64_t)y * 2 + 1 - cy2;

        for (x = x0; x < x1; x++) {
            int64_t dx2 = (int64_t)x * 2 + 1 - cx2;
            int64_t proj = dx2 * vx + dy2 * vy;
            int32_t pos = (int32_t)(((proj + L_span) * 1000LL) / (2 * L_span));
            uint32_t c;
            unsigned a;

            if (pos < 0) pos = 0;
            if (pos > 1000) pos = 1000;
            c = grad_sample(cs, pos);
            a = CSS_COLOR_A(c);
            if (!a)
                continue;
            a = scale_alpha(a, opacity);
            if (rounded) {
                unsigned ic = sd_to_cov(rrect_sd256(x, y, shape, rx, ry));
                if (opaque_bord) {
                    unsigned oc;
                    if (!ic) continue;
                    oc = sd_to_cov(rrect_sd256(x, y, br, r_out, r_out));
                    if (oc < 255u)
                        a = a * ic / 255u;
                } else {
                    a = a * ic / 255u;
                }
            }
            if (rc && rc->n > 0)
                a = a * rclip_cov(rc, x, y) / 255u;
            if (!a)
                continue;
            row[x] = a >= 255u ? (c & 0x00FFFFFFu) : paint_blend(row[x], c, a);
        }
    }
}

static void paint_box_shadow(const struct paint_target *t, lay_rect clip,
                             lay_rect br, lay_rect pr,
                             const int32_t r_out[4],
                             const int32_t rx_in[4], const int32_t ry_in[4],
                             int box_rounded, const struct computed_style *cs,
                             unsigned opacity, const struct rclip_list *rc)
{
    uint32_t scol = cs->shadow_color;
    unsigned base_a = scale_alpha(CSS_COLOR_A(scol), opacity);
    int32_t blur = cs->shadow_blur > 0 ? cs->shadow_blur : 0;
    int32_t sp = cs->shadow_spread;
    int32_t st, x0, y0, x1, y1, x, y;
    int32_t srad[4];
    int64_t b256;
    lay_rect sr, area, clipped;

    if (!t || !t->px || !base_a)
        return;

    b256 = (int64_t)blur * 256LL;
    if (cs->shadow_inset) {
        int i;

        sr = lay_mkrect(pr.x + cs->shadow_x + sp,
                        pr.y + cs->shadow_y + sp,
                        pr.w - 2 * sp,
                        pr.h - 2 * sp);
        for (i = 0; i < 4; i++) {
            int32_t rmin = pmin(rx_in[i], ry_in[i]);
            srad[i] = pmax(0, rmin - sp);
        }
        clipped = lay_rect_intersect(pr, clip);
    } else {
        int i;

        sr = lay_mkrect(br.x + cs->shadow_x - sp,
                        br.y + cs->shadow_y - sp,
                        br.w + 2 * sp,
                        br.h + 2 * sp);
        if (sr.w <= 0 || sr.h <= 0)
            return;
        for (i = 0; i < 4; i++)
            srad[i] = r_out[i] > 0 ? pmax(0, r_out[i] + sp) : 0;
        clamp_radii(sr, srad, srad);
        area = lay_mkrect(sr.x - blur, sr.y - blur,
                          sr.w + 2 * blur, sr.h + 2 * blur);
        clipped = lay_rect_intersect(area, clip);
    }

    x0 = pmax(0, clipped.x);
    y0 = pmax(0, clipped.y);
    x1 = pmin(t->w, clipped.x + clipped.w);
    y1 = pmin(t->h, clipped.y + clipped.h);
    if (x1 <= x0 || y1 <= y0)
        return;

    st = tstride(t);
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;

        for (x = x0; x < x1; x++) {
            int64_t sd;
            unsigned scov = 0, a;

            if (cs->shadow_inset) {
                unsigned in_cov = box_rounded
                    ? sd_to_cov(rrect_sd256(x, y, pr, rx_in, ry_in))
                    : 255u;

                if (!in_cov)
                    continue;
                if (sr.w <= 0 || sr.h <= 0) {
                    scov = 255;
                } else {
                    sd = rrect_sd256(x, y, sr, srad, srad);
                    if (blur <= 0) {
                        scov = sd >= 0 ? 255u : 0u;
                    } else if (sd <= -b256) {
                        scov = 0;
                    } else if (sd >= b256) {
                        scov = 255;
                    } else {
                        int64_t u = (sd + b256) * 255LL / (2 * b256);
                        scov = (unsigned)(u * u * (3 * 255 - 2 * u) /
                                          (255 * 255));
                    }
                }
                scov = scov * in_cov / 255u;
            } else {
                unsigned box_cov;

                if (box_rounded)
                    box_cov = sd_to_cov(rrect_sd256(x, y, br, r_out, r_out));
                else
                    box_cov = (x >= br.x && x < br.x + br.w &&
                               y >= br.y && y < br.y + br.h) ? 255u : 0u;
                if (box_cov >= 255u)
                    continue;
                sd = rrect_sd256(x, y, sr, srad, srad);
                if (blur <= 0) {
                    scov = sd_to_cov(sd);
                } else if (sd <= -b256) {
                    scov = 255;
                } else if (sd >= b256) {
                    scov = 0;
                } else {
                    int64_t u = (b256 - sd) * 255LL / (2 * b256);
                    scov = (unsigned)(u * u * (3 * 255 - 2 * u) /
                                      (255 * 255));
                }
                scov = scov * (255u - box_cov) / 255u;
            }
            if (!scov)
                continue;
            a = base_a * scov / 255u;
            if (rc && rc->n > 0)
                a = a * rclip_cov(rc, x, y) / 255u;
            if (!a)
                continue;
            row[x] = a >= 255u ? (scol & 0x00FFFFFFu)
                               : paint_blend(row[x], scol, a);
        }
    }
}

static void paint_frame_rounded(const struct paint_target *t, lay_rect clip,
                                lay_rect br, lay_rect pr,
                                const int32_t r_out[4],
                                const int32_t rx_in[4], const int32_t ry_in[4],
                                const int32_t bw[4], const uint32_t col[4],
                                const uint8_t sty[4], unsigned opacity,
                                const struct rclip_list *rc)
{
    int32_t x0, y0, x1, y1, x, y, st;
    int32_t ix0, iy0, ix1, iy1;
    lay_rect r;

    if (!t || !t->px || !opacity)
        return;
    r = lay_rect_intersect(br, clip);
    x0 = pmax(0, r.x);
    y0 = pmax(0, r.y);
    x1 = pmin(t->w, r.x + r.w);
    y1 = pmin(t->h, r.y + r.h);
    if (x1 <= x0 || y1 <= y0)
        return;

    /* Strict rectangular interior where neither borders nor corner curves reach */
    ix0 = pr.x + pmax(rx_in[CSS_TL], rx_in[CSS_BL]) + 1;
    iy0 = pr.y + pmax(ry_in[CSS_TL], ry_in[CSS_TR]) + 1;
    ix1 = pr.x + pr.w - pmax(rx_in[CSS_TR], rx_in[CSS_BR]) - 1;
    iy1 = pr.y + pr.h - pmax(ry_in[CSS_BL], ry_in[CSS_BR]) - 1;

    st = tstride(t);
    for (y = y0; y < y1; y++) {
        uint32_t *row = t->px + (long)y * st;

        for (x = x0; x < x1; x++) {
            unsigned oc, ic, bc, a;
            int side = -1;
            int64_t best = 0x3FFFFFFFFFFFFFFFLL;

            if (x >= ix0 && x < ix1 && y >= iy0 && y < iy1) {
                x = ix1 - 1;
                continue;
            }
            oc = sd_to_cov(rrect_sd256(x, y, br, r_out, r_out));
            if (!oc)
                continue;
            ic = (pr.w > 0 && pr.h > 0)
                ? sd_to_cov(rrect_sd256(x, y, pr, rx_in, ry_in)) : 0u;
            bc = oc > ic ? (oc - ic) : 0u;
            if (!bc)
                continue;

            if (bw[CSS_TOP] > 0) {
                int64_t d = (int64_t)(y - br.y) * 1024LL / bw[CSS_TOP];
                if (d < best) { best = d; side = CSS_TOP; }
            }
            if (bw[CSS_BOTTOM] > 0) {
                int64_t d = (int64_t)(br.y + br.h - 1 - y) * 1024LL /
                            bw[CSS_BOTTOM];
                if (d < best) { best = d; side = CSS_BOTTOM; }
            }
            if (bw[CSS_LEFT] > 0) {
                int64_t d = (int64_t)(x - br.x) * 1024LL / bw[CSS_LEFT];
                if (d < best) { best = d; side = CSS_LEFT; }
            }
            if (bw[CSS_RIGHT] > 0) {
                int64_t d = (int64_t)(br.x + br.w - 1 - x) * 1024LL /
                            bw[CSS_RIGHT];
                if (d < best) { best = d; side = CSS_RIGHT; }
            }
            if (side < 0 || sty[side] == CSS_BORDERSTYLE_NONE ||
                sty[side] == CSS_BORDERSTYLE_HIDDEN)
                continue;
            a = scale_alpha(CSS_COLOR_A(col[side]), opacity) * bc / 255u;
            if (rc && rc->n > 0)
                a = a * rclip_cov(rc, x, y) / 255u;
            if (!a)
                continue;
            row[x] = a >= 255u ? (col[side] & 0x00FFFFFFu)
                               : paint_blend(row[x], col[side], a);
        }
    }
}

static void paint_outline(struct pstate *p, lay_rect clip, lay_rect br,
                          const int32_t r_out[4], int rounded,
                          const struct computed_style *cs,
                          unsigned opacity, const struct rclip_list *rc)
{
    int32_t ow = cs->outline_width;
    int32_t off = cs->outline_offset;
    uint8_t sty = cs->outline_style;
    uint32_t ocol = cs->outline_color;
    lay_rect out_r, in_r;
    int32_t obw[4], ro[4], ri[4];
    uint32_t cols[4];
    uint8_t stys[4];
    int i;

    if (ow <= 0 || sty == CSS_BORDERSTYLE_NONE ||
        sty == CSS_BORDERSTYLE_HIDDEN || !CSS_COLOR_A(ocol))
        return;

    out_r = lay_mkrect(br.x - off - ow, br.y - off - ow,
                       br.w + 2 * (off + ow), br.h + 2 * (off + ow));
    in_r = lay_mkrect(br.x - off, br.y - off,
                      br.w + 2 * off, br.h + 2 * off);
    if (out_r.w <= 0 || out_r.h <= 0)
        return;

    for (i = 0; i < 4; i++) {
        obw[i] = ow;
        cols[i] = ocol;
        stys[i] = sty;
        ro[i] = rounded && r_out[i] > 0 ? pmax(0, r_out[i] + off + ow) : 0;
        ri[i] = rounded && r_out[i] > 0 ? pmax(0, r_out[i] + off) : 0;
    }
    if (rounded && sty == CSS_BORDERSTYLE_SOLID) {
        paint_frame_rounded(p->t, clip, out_r, in_r, ro, ri, ri,
                            obw, cols, stys, opacity, rc);
    } else if (opacity == 1000 && rc->n == 0) {
        paint_frame(p->t, clip, out_r, ow, ow, ow, ow, cols, stys);
    } else {
        uint32_t fcol[4];
        for (i = 0; i < 4; i++) {
            unsigned ca = scale_alpha(CSS_COLOR_A(ocol), opacity);
            fcol[i] = (ca << 24) | (ocol & 0x00FFFFFFu);
        }
        if (rc->n == 0)
            paint_frame(p->t, clip, out_r, ow, ow, ow, ow, fcol, stys);
        else
            paint_frame_rounded(p->t, clip, out_r, in_r, ro, ri, ri,
                                obw, cols, stys, opacity, rc);
    }
    if (p->st) p->st->rects++;
}

static void paint_background(struct pstate *p, const struct lay_box *b)
{
    const struct computed_style *cs = b->style;
    int32_t bx, by;
    lay_rect clip, br, pr;
    unsigned opacity;
    struct rclip_list rc;
    int rounded, has_bord;
    int32_t r_out[4], rx_in[4], ry_in[4];
    uint32_t col[4];
    uint8_t sty[4];
    int i;

    if (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_LINE)
        return;
    if (b->flags & LAYF_HIDDEN)
        return;

    opacity = effective_opacity(b);
    if (!opacity)
        return;

    box_shift(p, b, &bx, &by);
    clip = lay_rect_intersect(box_self_clip_rect(p, b, bx, by), p->dirty);
    if (clip.w <= 0 || clip.h <= 0)
        return;
    br = shift_rect(lay_border_rect(b), bx, by);
    pr = shift_rect(lay_padding_rect(b), bx, by);
    collect_rclips(p, b, 0, &rc);

    rounded = has_any_radius(cs->border_radius);
    if (rounded) {
        clamp_radii(br, cs->border_radius, r_out);
        box_inner_radii(b, br, rx_in, ry_in);
    } else {
        r_out[0] = r_out[1] = r_out[2] = r_out[3] = 0;
        rx_in[0] = rx_in[1] = rx_in[2] = rx_in[3] = 0;
        ry_in[0] = ry_in[1] = ry_in[2] = ry_in[3] = 0;
    }

    has_bord = b->bord[CSS_TOP] || b->bord[CSS_RIGHT] ||
               b->bord[CSS_BOTTOM] || b->bord[CSS_LEFT];
    for (i = 0; i < 4; i++) {
        col[i] = cs->border_color[i];
        if (!CSS_COLOR_A(col[i]))
            col[i] = cs->color;
        sty[i] = cs->border_style[i];
    }

    if (cs->has_shadow && !cs->shadow_inset && CSS_COLOR_A(cs->shadow_color)) {
        paint_box_shadow(p->t, clip, br, pr, r_out, rx_in, ry_in,
                         rounded, cs, opacity, &rc);
        if (p->st) p->st->rects++;
    }

    /* Fast exact path when no radius, full opacity, no gradient, no rclip */
    if (!rounded && opacity == 1000 && rc.n == 0 && !cs->has_gradient) {
        if (CSS_COLOR_A(cs->background_color)) {
            paint_fill(p->t, clip, pr, cs->background_color);
            if (p->st) p->st->rects++;
        }
        if (cs->background_image && p->o->image) {
            const struct image *im = p->o->image(p->o->image_ctx,
                                                 cs->background_image);
            if (im) {
                blit_image(p->t, lay_rect_intersect(clip, pr), pr, im);
                if (p->st) p->st->images++;
            }
        }
        if (cs->has_shadow && cs->shadow_inset &&
            CSS_COLOR_A(cs->shadow_color)) {
            paint_box_shadow(p->t, clip, br, pr, r_out, rx_in, ry_in,
                             0, cs, 1000, 0);
            if (p->st) p->st->rects++;
        }
        if (has_bord) {
            paint_frame(p->t, clip, br, b->bord[CSS_TOP], b->bord[CSS_RIGHT],
                        b->bord[CSS_BOTTOM], b->bord[CSS_LEFT], col, sty);
            if (p->st) p->st->rects++;
        }
    } else {
        /* When rounded + borders: use padding-box inner radii for bg if
         * borders are present, or outer border-box radii if borderless.
         * Note: to avoid AA canvas bleed at the inner border edge when
         * borders are opaque solid, fill bg up to pr+1px inside br! */
        lay_rect bg_shape = pr;
        const int32_t *bg_rx = rx_in, *bg_ry = ry_in;
        int opaque_solid_bord = has_bord && opacity == 1000;

        if (opaque_solid_bord) {
            for (i = 0; i < 4; i++) {
                if (b->bord[i] > 0 &&
                    (sty[i] != CSS_BORDERSTYLE_SOLID ||
                     CSS_COLOR_A(col[i]) < 255))
                    opaque_solid_bord = 0;
            }
        }
        if (!has_bord) {
            bg_shape = br;
            bg_rx = r_out;
            bg_ry = r_out;
        }
        if (CSS_COLOR_A(cs->background_color)) {
            if (rounded) {
                if (opaque_solid_bord) {
                    /* Paint bg with inner cov OR 255 wherever inside outer br
                     * & touching inner pr so solid border blends onto bg. */
                    int32_t x0, y0, x1, y1, x, y, st;
                    unsigned ba = CSS_COLOR_A(cs->background_color);
                    lay_rect rr = lay_rect_intersect(br, clip);

                    x0 = pmax(0, rr.x); y0 = pmax(0, rr.y);
                    x1 = pmin(p->t->w, rr.x + rr.w);
                    y1 = pmin(p->t->h, rr.y + rr.h);
                    st = tstride(p->t);
                    for (y = y0; y < y1; y++) {
                        uint32_t *row = p->t->px + (long)y * st;
                        for (x = x0; x < x1; x++) {
                            unsigned ic = (pr.w > 0 && pr.h > 0)
                                ? sd_to_cov(rrect_sd256(x, y, pr, rx_in, ry_in))
                                : 0u;
                            unsigned oc, ca;
                            if (!ic) continue;
                            oc = sd_to_cov(rrect_sd256(x, y, br, r_out, r_out));
                            ca = oc >= 255u ? ba : (ba * ic / 255u);
                            if (rc.n > 0)
                                ca = ca * rclip_cov(&rc, x, y) / 255u;
                            if (!ca) continue;
                            row[x] = ca >= 255u
                                ? (cs->background_color & 0x00FFFFFFu)
                                : paint_blend(row[x], cs->background_color, ca);
                        }
                    }
                } else {
                    paint_fill_rrect(p->t, clip, bg_shape, bg_rx, bg_ry,
                                     cs->background_color, opacity, &rc);
                }
            } else {
                paint_fill_ex(p->t, clip, pr, cs->background_color,
                              opacity, &rc);
            }
            if (p->st) p->st->rects++;
        }
        if (cs->has_gradient && cs->grad_nstops > 0) {
            if (rounded && opaque_solid_bord) {
                paint_grad_box(p->t, clip, pr, pr, rx_in, ry_in, 1, 1, br,
                               r_out, cs, opacity, &rc);
            } else {
                paint_grad_box(p->t, clip, pr, bg_shape, bg_rx, bg_ry, rounded,
                               0, br, r_out, cs, opacity, &rc);
            }
            if (p->st) p->st->rects++;
        }
        if (cs->background_image && p->o->image) {
            const struct image *im = p->o->image(p->o->image_ctx,
                                                 cs->background_image);
            if (im) {
                if (rounded) {
                    struct rclip_list irc = rc;
                    if (irc.n < RCLIP_MAX) {
                        struct rclip_entry *e = &irc.e[irc.n++];
                        e->rect = bg_shape;
                        memcpy(e->rx, bg_rx, sizeof(e->rx));
                        memcpy(e->ry, bg_ry, sizeof(e->ry));
                    }
                    blit_image_ex(p->t, lay_rect_intersect(clip, pr), pr,
                                  im, opacity, &irc);
                } else {
                    blit_image_ex(p->t, lay_rect_intersect(clip, pr), pr,
                                  im, opacity, &rc);
                }
                if (p->st) p->st->images++;
            }
        }
        if (cs->has_shadow && cs->shadow_inset &&
            CSS_COLOR_A(cs->shadow_color)) {
            paint_box_shadow(p->t, clip, br, pr, r_out, rx_in, ry_in,
                             rounded, cs, opacity, &rc);
            if (p->st) p->st->rects++;
        }
        if (has_bord) {
            if (rounded) {
                paint_frame_rounded(p->t, clip, br, pr, r_out, rx_in, ry_in,
                                    b->bord, col, sty, opacity, &rc);
            } else if (opacity == 1000 && rc.n == 0) {
                paint_frame(p->t, clip, br, b->bord[CSS_TOP], b->bord[CSS_RIGHT],
                            b->bord[CSS_BOTTOM], b->bord[CSS_LEFT], col, sty);
            } else {
                uint32_t ocol[4];
                for (i = 0; i < 4; i++) {
                    unsigned ca = scale_alpha(CSS_COLOR_A(col[i]), opacity);
                    ocol[i] = (ca << 24) | (col[i] & 0x00FFFFFFu);
                }
                if (rc.n == 0)
                    paint_frame(p->t, clip, br, b->bord[CSS_TOP],
                                b->bord[CSS_RIGHT], b->bord[CSS_BOTTOM],
                                b->bord[CSS_LEFT], ocol, sty);
                else
                    paint_frame_rounded(p->t, clip, br, pr, r_out,
                                        rx_in, ry_in, b->bord, col, sty,
                                        opacity, &rc);
            }
            if (p->st) p->st->rects++;
        }
    }
    paint_outline(p, clip, br, r_out, rounded, cs, opacity, &rc);
    if (p->o->debug_boxes) {
        for (i = 0; i < 4; i++) {
            col[i] = 0xFFFF00FFu;
            sty[i] = CSS_BORDERSTYLE_SOLID;
        }
        paint_frame(p->t, clip, br, 1, 1, 1, 1, col, sty);
    }
}

static int value_ci(const char *a, const char *b)
{
    if (!a)
        return 0;
    while (*a && *b) {
        int ca = (unsigned char)*a++;
        int cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int paint_form_control(struct pstate *p, const struct lay_box *b,
                              lay_rect clip, lay_rect cr, uint32_t fg)
{
    struct dom_node *n = b->node;
    const char *type, *text = 0;
    char masked[129];
    char *owned = 0;
    const struct font *f;
    font_surface fs;
    font_rc inner;

    if (!n || n->type != DOM_ELEMENT ||
        (n->tag_id != HTAG_INPUT && n->tag_id != HTAG_TEXTAREA &&
         n->tag_id != HTAG_SELECT))
        return 0;
    type = n->tag_id == HTAG_INPUT ? dom_get_attr(n, "type") : 0;
    if (!type || !*type) type = "text";
    if (n->tag_id == HTAG_INPUT && value_ci(type, "image"))
        return 0;

    if (n->tag_id == HTAG_INPUT &&
        (value_ci(type, "checkbox") || value_ci(type, "radio"))) {
        lay_rect mark = lay_mkrect(cr.x + 2, cr.y + 2,
                                  pmax(1, cr.w - 4), pmax(1, cr.h - 4));
        uint32_t c = 0xFF000000u | fg;

        if (dom_has_attr(n, "checked")) {
            if (value_ci(type, "radio")) {
                int32_t size = pmin(mark.w, mark.h) / 2;
                paint_fill(p->t, clip,
                           lay_mkrect(mark.x + (mark.w - size) / 2,
                                      mark.y + (mark.h - size) / 2,
                                      size, size), c);
            } else {
                int32_t i, span = pmin(mark.w, mark.h);
                for (i = 0; i < span; i++) {
                    paint_fill(p->t, clip,
                               lay_mkrect(mark.x + i, mark.y + i, 1, 1), c);
                    paint_fill(p->t, clip,
                               lay_mkrect(mark.x + mark.w - 1 - i,
                                          mark.y + i, 1, 1), c);
                }
            }
        }
        return 1;
    }
    if (n->tag_id == HTAG_INPUT && value_ci(type, "range")) {
        int32_t cy = cr.y + cr.h / 2;
        paint_fill(p->t, clip, lay_mkrect(cr.x + 4, cy - 1,
                                         pmax(1, cr.w - 8), 3),
                   0xFF808080u);
        paint_fill(p->t, clip, lay_mkrect(cr.x + cr.w / 2 - 3, cy - 6,
                                         7, 13), 0xFF404040u);
        return 1;
    }
    if (n->tag_id == HTAG_SELECT) {
        struct dom_node *o;

        for (o = n->first_child; o; o = dom_next_within(o, n))
            if (o->type == DOM_ELEMENT && o->tag_id == HTAG_OPTION &&
                dom_has_attr(o, "selected")) {
                owned = dom_text_content(o, 0);
                break;
            }
        if (!owned)
            for (o = n->first_child; o; o = dom_next_within(o, n))
                if (o->type == DOM_ELEMENT && o->tag_id == HTAG_OPTION) {
                    owned = dom_text_content(o, 0);
                    break;
                }
        text = owned ? owned : "";
    } else if (n->tag_id == HTAG_TEXTAREA) {
        text = dom_get_attr(n, "value");
        if (!text) {
            owned = dom_text_content(n, 0);
            text = owned ? owned : "";
        }
    } else {
        text = dom_get_attr(n, "value");
        if ((!text || !*text) && value_ci(type, "submit"))
            text = "Submit";
        else if ((!text || !*text) && value_ci(type, "reset"))
            text = "Reset";
        else if (!text || !*text)
            text = dom_get_attr(n, "placeholder");
        if (!text) text = "";
        if (value_ci(type, "password")) {
            unsigned long i, len = strlen(text);
            if (len > sizeof(masked) - 1) len = sizeof(masked) - 1;
            for (i = 0; i < len; i++) masked[i] = '*';
            masked[len] = 0;
            text = masked;
        }
    }
    f = lay_font_for(b->style);
    fs = font_surface_of(p->t->px, p->t->w, p->t->h, tstride(p->t));
    inner = font_mkrc(pmax(clip.x, cr.x + 3), pmax(clip.y, cr.y + 2),
                      pmin(clip.w, pmax(0, cr.w - 6)),
                      pmin(clip.h, pmax(0, cr.h - 4)));
    if (inner.w > 0 && inner.h > 0)
        font_draw_run(&fs, inner, cr.x + 4,
                      cr.y + pmax(1, (cr.h - f->h) / 2),
                      text, -1, f, fg, FONT_TRANSPARENT,
                      pmax(0, cr.w - 8), 1, 0);
    if (n->tag_id == HTAG_SELECT && cr.w >= 12) {
        int32_t ax = cr.x + cr.w - 9, ay = cr.y + cr.h / 2 - 1, i;
        for (i = 0; i < 4; i++)
            paint_fill(p->t, clip, lay_mkrect(ax + i, ay + i, 7 - i * 2, 1),
                       0xFF000000u | fg);
    }
    free(owned);
    return 1;
}

static uint32_t *snapshot_region(const struct paint_target *t, lay_rect r)
{
    uint32_t *buf;
    int32_t y, st;

    if (r.w <= 0 || r.h <= 0)
        return 0;
    if ((uint64_t)r.w * (uint64_t)r.h > 4UL * 1024UL * 1024UL)
        return 0;
    buf = (uint32_t *)malloc((unsigned long)r.w * (unsigned long)r.h *
                             sizeof(uint32_t));
    if (!buf)
        return 0;
    st = tstride(t);
    for (y = 0; y < r.h; y++)
        memcpy(buf + (long)y * r.w,
               t->px + (long)(r.y + y) * st + r.x,
               (unsigned long)r.w * sizeof(uint32_t));
    return buf;
}

static void composite_snapshot(const struct paint_target *t, lay_rect r,
                               uint32_t *saved, unsigned opacity,
                               const struct rclip_list *rc)
{
    int32_t x, y, st = tstride(t);

    if (!saved)
        return;
    for (y = 0; y < r.h; y++) {
        uint32_t *row = t->px + (long)(r.y + y) * st + r.x;
        const uint32_t *srow = saved + (long)y * r.w;

        for (x = 0; x < r.w; x++) {
            unsigned c = opacity < 1000 ? scale_alpha(255u, opacity) : 255u;

            if (rc && rc->n > 0)
                c = c * rclip_cov(rc, r.x + x, r.y + y) / 255u;
            if (c < 255u)
                row[x] = paint_blend(srow[x], row[x], c);
        }
    }
    free(saved);
}

static lay_rect object_fit_rect(lay_rect cr, int32_t iw, int32_t ih, int fit)
{
    int32_t dw = cr.w, dh = cr.h;
    int32_t cw, ch;
    int64_t lhs, rhs;

    if (iw <= 0 || ih <= 0 || cr.w <= 0 || cr.h <= 0 ||
        fit == CSS_OBJECTFIT_FILL)
        return cr;
    if (fit == CSS_OBJECTFIT_NONE) {
        return lay_mkrect(cr.x + (cr.w - iw) / 2, cr.y + (cr.h - ih) / 2,
                          iw, ih);
    }
    lhs = (int64_t)iw * cr.h;
    rhs = (int64_t)cr.w * ih;
    if (fit == CSS_OBJECTFIT_COVER) {
        if (lhs >= rhs) {
            dh = cr.h;
            dw = (int32_t)(((int64_t)cr.h * iw + ih / 2) / ih);
        } else {
            dw = cr.w;
            dh = (int32_t)(((int64_t)cr.w * ih + iw / 2) / iw);
        }
        return lay_mkrect(cr.x + (cr.w - dw) / 2, cr.y + (cr.h - dh) / 2,
                          dw, dh);
    }
    if (lhs >= rhs) {
        cw = cr.w;
        ch = (int32_t)(((int64_t)cr.w * ih + iw / 2) / iw);
    } else {
        ch = cr.h;
        cw = (int32_t)(((int64_t)cr.h * iw + ih / 2) / ih);
    }
    if (fit == CSS_OBJECTFIT_SCALE_DOWN && iw <= cw && ih <= ch) {
        dw = iw;
        dh = ih;
    } else {
        dw = cw;
        dh = ch;
    }
    return lay_mkrect(cr.x + (cr.w - dw) / 2, cr.y + (cr.h - dh) / 2,
                      dw, dh);
}

static void paint_text_span(const font_surface *fs, font_rc frc,
                            int32_t x, int32_t y, const char *s, int n,
                            const struct font *f, int32_t ls, uint32_t fg)
{
    int i;
    int32_t step;

    if (n <= 0 || !f)
        return;
    if (ls == 0) {
        font_draw_text_n(fs, frc, x, y, s, n, f, fg, FONT_TRANSPARENT);
        return;
    }
    step = f->w + ls;
    if (step < 0)
        step = 0;
    for (i = 0; i < n && s[i]; i++) {
        font_draw_char(fs, frc, x, y, (unsigned char)s[i], f, fg,
                       FONT_TRANSPARENT);
        x += step;
    }
}

static void paint_content(struct pstate *p, const struct lay_box *b)
{
    const struct computed_style *cs = b->style;
    int32_t bx, by;
    lay_rect clip;
    font_surface fs;
    font_rc frc;
    uint32_t fg;
    unsigned opacity;
    struct rclip_list rc;
    lay_rect snap_r = {0, 0, 0, 0};
    uint32_t *saved = 0;
    int need_snap = 0;

    if (b->flags & LAYF_HIDDEN)
        return;
    opacity = effective_opacity(b);
    if (!opacity)
        return;

    box_shift(p, b, &bx, &by);
    clip = lay_rect_intersect(box_clip_rect(p, b, bx, by), p->dirty);
    fg = CSS_COLOR_RGB(cs->color);
    if ((b->flags & LAYF_LINK) && p->o->link_color)
        fg = CSS_COLOR_RGB(p->o->link_color);

    fs = font_surface_of(p->t->px, p->t->w, p->t->h, tstride(p->t));
    frc = font_mkrc(clip.x, clip.y, clip.w, clip.h);
    if (frc.w <= 0 || frc.h <= 0)
        return;

    collect_rclips(p, b, b->kind == LAY_BOX_REPLACED, &rc);

    switch (b->kind) {
    case LAY_BOX_REPLACED: {
        lay_rect cr = shift_rect(lay_content_rect(b), bx, by);
        struct image canvas_im;
        const struct image *im = (b->src && p->o->image)
            ? p->o->image(p->o->image_ctx, b->src) : 0;

        if (!im && b->node && b->node->tag_id == HTAG_CANVAS &&
            b->node->canvas && b->node->canvas->px &&
            b->node->canvas->w > 0 && b->node->canvas->h > 0) {
            canvas_im.w = b->node->canvas->w;
            canvas_im.h = b->node->canvas->h;
            canvas_im.px = b->node->canvas->px;
            canvas_im.has_alpha = (b->node->canvas->alpha != 0);
            canvas_im.alpha = b->node->canvas->alpha;
            im = &canvas_im;
        }

        if (b->node && (b->node->tag_id == HTAG_INPUT ||
                        b->node->tag_id == HTAG_TEXTAREA ||
                        b->node->tag_id == HTAG_SELECT)) {
            if (opacity < 1000 || rclip_touches_corners(&rc, cr)) {
                snap_r = lay_rect_intersect(cr, lay_rect_intersect(clip,
                         lay_mkrect(0, 0, p->t->w, p->t->h)));
                saved = snapshot_region(p->t, snap_r);
            }
            if (paint_form_control(p, b, clip, cr, fg)) {
                if (saved)
                    composite_snapshot(p->t, snap_r, saved, opacity, &rc);
                if (p->st) p->st->rects++;
                return;
            }
            free(saved);
            saved = 0;
        }
        if (im) {
            lay_rect dst = object_fit_rect(cr, im->w, im->h, cs->object_fit);
            lay_rect im_clip = lay_rect_intersect(clip, cr);
            blit_image_ex(p->t, im_clip, dst, im, opacity, &rc);
            if (p->st) p->st->images++;
        } else if (b->node && b->node->tag_id == HTAG_SVG) {
            if (rc.n > 0 && rclip_touches_corners(&rc, cr)) {
                snap_r = lay_rect_intersect(cr, lay_rect_intersect(clip,
                         lay_mkrect(0, 0, p->t->w, p->t->h)));
                saved = snapshot_region(p->t, snap_r);
            }
            if (paint_inline_svg(p->t, clip, cr, b->node, cs->color,
                                 opacity)) {
                if (saved)
                    composite_snapshot(p->t, snap_r, saved, 1000, &rc);
                if (p->st) p->st->images++;
                return;
            }
            free(saved);
            saved = 0;
        }
        if (!(im) && !(b->node && b->node->tag_id == HTAG_SVG)) {
            /* The placeholder layout reserved: a hairline box with the
             * alt text in it, so a page with broken images still reads. */
            uint32_t col[4];
            uint8_t sty[4];
            const char *alt = b->node ? dom_get_attr(b->node, "alt") : 0;
            int i;

            if ((!alt || !*alt) && b->node &&
                b->node->tag_id == HTAG_VIDEO)
                alt = "Video";
            if ((!alt || !*alt) && b->node &&
                b->node->tag_id == HTAG_AUDIO)
                alt = "Audio";
            for (i = 0; i < 4; i++) {
                col[i] = 0xFF000000u | shade(fg, -20);
                sty[i] = CSS_BORDERSTYLE_SOLID;
            }
            if (cr.w > 1 && cr.h > 1)
                paint_frame(p->t, clip, cr, 1, 1, 1, 1, col, sty);
            if (alt && *alt && cr.w > 8) {
                const struct font *f = lay_font_for(cs);
                font_rc inner = font_mkrc(pmax(clip.x, cr.x + 2),
                                          pmax(clip.y, cr.y + 2),
                                          pmin(clip.w, cr.w - 4),
                                          pmin(clip.h, cr.h - 4));

                font_draw_run(&fs, inner, cr.x + 4, cr.y + 2, alt, -1, f,
                              fg, FONT_TRANSPARENT, cr.w - 8, 1, 0);
            }
        }
        return;
    }
    case LAY_BOX_MARKER: {
        lay_rect r = shift_rect(lay_mkrect(b->x, b->y, b->w, b->h), bx, by);

        need_snap = (opacity < 1000) || rclip_touches_corners(&rc, r);
        if (need_snap) {
            snap_r = lay_rect_intersect(r, lay_rect_intersect(clip,
                     lay_mkrect(0, 0, p->t->w, p->t->h)));
            saved = snapshot_region(p->t, snap_r);
        }
        if (b->marker)
            font_draw_text_n(&fs, frc, r.x, r.y, b->marker, -1,
                             b->font ? b->font : lay_font_for(cs), fg,
                             FONT_TRANSPARENT);
        else
            draw_marker_glyph(p->t, clip, b, bx, by,
                              0xFF000000u | fg);
        if (saved)
            composite_snapshot(p->t, snap_r, saved, opacity, &rc);
        if (p->st) p->st->glyphs++;
        return;
    }
    case LAY_BOX_TEXT: {
        lay_rect r = shift_rect(lay_mkrect(b->x, b->y, b->w, b->h), bx, by);
        uint32_t s0 = 0, s1 = 0, sfg = 0, sbg = 0;
        int sel = 0;

        if (!b->font || !b->text_len)
            return;
        /* Include italic overhang in snapshot bounds if italic */
        {
            lay_rect bound = lay_mkrect(r.x - 4, r.y, r.w + 8, r.h + 2);
            need_snap = (opacity < 1000) || rclip_touches_corners(&rc, bound);
            if (need_snap) {
                snap_r = lay_rect_intersect(bound, lay_rect_intersect(clip,
                         lay_mkrect(0, 0, p->t->w, p->t->h)));
                saved = snapshot_region(p->t, snap_r);
            }
        }
        if (p->o->selection)
            sel = p->o->selection(p->o->selection_ctx, b, &s0, &s1,
                                  &sfg, &sbg);
        if (sel) {
            if (s1 > b->text_len) s1 = b->text_len;
            if (s0 > s1) s0 = s1;
        }
        {
            int32_t ls = cs ? cs->letter_spacing : 0;
            int32_t step = (b->font->w + ls > 0) ? (b->font->w + ls) : 0;

            if (sel && s1 > s0) {
                int32_t x0 = r.x + (int32_t)s0 * step;
                int32_t x1 = r.x + (int32_t)s1 * step;
                int32_t sel_w = (s1 == b->text_len && ls > 0)
                    ? (r.x + r.w - x0) : (x1 - x0);

                if (sel_w < b->font->w) sel_w = b->font->w;
                paint_fill(p->t, clip, lay_mkrect(x0, r.y, sel_w, r.h),
                           CSS_COLOR_A(sbg) ? sbg : 0xFF5BA3D0u);
                paint_text_span(&fs, frc, r.x, r.y, b->text, (int)s0,
                                b->font, ls, fg);
                paint_text_span(&fs, frc, x0, r.y, b->text + s0,
                                (int)(s1 - s0), b->font, ls,
                                CSS_COLOR_A(sfg) ? CSS_COLOR_RGB(sfg)
                                                 : 0xFFFFFFu);
                paint_text_span(&fs, frc, x1, r.y, b->text + s1,
                                (int)(b->text_len - s1), b->font, ls, fg);
            } else {
                paint_text_span(&fs, frc, r.x, r.y, b->text,
                                (int)b->text_len, b->font, ls, fg);
            }
            if (p->st) p->st->glyphs += b->text_len;
            if (cs->text_decoration)
                draw_decorations(p->t, clip, b, r.x, r.y, b->w,
                                 0xFF000000u | fg);
            if (p->o->caret_box == b) {
                uint32_t off_c = p->o->caret_offset > b->text_len
                    ? b->text_len : p->o->caret_offset;
                int32_t cx = r.x + (int32_t)off_c * step;

                paint_fill(p->t, clip, lay_mkrect(cx, r.y, 1, r.h),
                           p->o->caret_color ? p->o->caret_color
                                             : 0xFF000000u);
            }
        }
        if (saved)
            composite_snapshot(p->t, snap_r, saved, opacity, &rc);
        return;
    }
    default:
        return;
    }
}

int lay_paint(const struct lay_document *L, const struct paint_target *t,
              lay_rect dirty, const struct paint_opts *o,
              struct paint_stats *stats)
{
    const struct lay_paint_item *items;
    struct paint_opts def;
    struct pstate p;
    int n, i, drawn = 0;

    if (!L || !t || !t->px)
        return 0;
    if (!o) {
        paint_opts_init(&def);
        def.canvas = lay_canvas_color(L);
        o = &def;
    }
    if (stats)
        memset(stats, 0, sizeof *stats);

    dirty = lay_rect_intersect(dirty, lay_mkrect(0, 0, t->w, t->h));
    if (dirty.w <= 0 || dirty.h <= 0)
        return 0;

    p.L = L;
    p.t = t;
    p.o = o;
    p.st = stats;
    p.dirty = dirty;
    p.dx = o->origin_x - o->scroll_x;
    p.dy = o->origin_y - o->scroll_y;

    if (CSS_COLOR_A(o->canvas))
        paint_fill(t, dirty, dirty, o->canvas);

    n = lay_paint_order(L, &items);
    for (i = 0; i < n; i++) {
        const struct lay_box *b = items[i].box;
        int32_t bx, by;
        lay_rect ink, own, cl;

        if (stats)
            stats->items_considered++;
        box_shift(&p, b, &bx, &by);
        ink = shift_rect(lay_ink_rect(b), bx, by);
        /* Two O(1) rejections: the subtree ink extent, then the box. */
        if (!lay_rect_intersects(ink, dirty))
            continue;
        own = (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_MARKER)
            ? shift_rect(lay_mkrect(b->x, b->y, b->w, b->h), bx, by)
            : shift_rect(lay_paint_rect(b), bx, by);
        if (!lay_rect_intersects(own, dirty))
            continue;
        cl = (items[i].phase == LAY_PHASE_BACKGROUND)
            ? box_self_clip_rect(&p, b, bx, by)
            : box_clip_rect(&p, b, bx, by);
        if (!lay_rect_intersects(cl, dirty))
            continue;

        if (items[i].phase == LAY_PHASE_BACKGROUND)
            paint_background(&p, b);
        else
            paint_content(&p, b);
        drawn++;
        if (stats)
            stats->items_drawn++;
    }

    if (o->highlight_box) {
        int32_t bx, by;
        lay_rect r;
        uint32_t col[4];
        uint8_t sty[4];
        int k;

        box_shift(&p, o->highlight_box, &bx, &by);
        r = shift_rect(lay_border_rect(o->highlight_box), bx, by);
        for (k = 0; k < 4; k++) {
            col[k] = o->highlight_color;
            sty[k] = CSS_BORDERSTYLE_SOLID;
        }
        paint_frame(t, dirty, r, 1, 1, 1, 1, col, sty);
    }
    return drawn;
}

/* ---------------------------------------------------------------- ppm */

#ifdef PAINT_WITH_STDIO
#include <stdio.h>

int paint_write_ppm(const struct paint_target *t, const char *path)
{
    FILE *f;
    int32_t x, y, st;

    if (!t || !t->px || !path)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", (int)t->w, (int)t->h);
    st = tstride(t);
    for (y = 0; y < t->h; y++) {
        const uint32_t *row = t->px + (long)y * st;

        for (x = 0; x < t->w; x++) {
            unsigned char rgb[3];
            uint32_t c = row[x];

            rgb[0] = (unsigned char)((c >> 16) & 0xFF);
            rgb[1] = (unsigned char)((c >> 8) & 0xFF);
            rgb[2] = (unsigned char)(c & 0xFF);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}
#else
int paint_write_ppm(const struct paint_target *t, const char *path)
{
    (void)t;
    (void)path;
    return -1;
}
#endif
