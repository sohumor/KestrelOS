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

#define SVG_SHAPE_MAX 512
#define SVG_PIXEL_MAX (4UL * 1024UL * 1024UL)
#define SVG_WORK_MAX  (4UL * 1024UL * 1024UL)
#define SVG_COORD_MAX 1000000000LL
#define SVG_ELLIPSE_SCALE 1048576LL

struct svg_painter {
    const struct paint_target *t;
    lay_rect clip, dst;
    int64_t minx, miny, vieww, viewh; /* thousandths of a user unit */
    int shapes;
    unsigned long pixels, work;
};

struct svg_style {
    uint32_t fill, stroke;       /* AARRGGBB; alpha zero means none */
    int32_t stroke_width;        /* thousandths of a user unit */
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

static uint32_t svg_color(const char *s, uint32_t fallback)
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
    unsigned long i;

    if (!s || !*s)
        return fallback;
    while (*s == ' ' || *s == '\t') s++;
    if (svg_ieq(s, "none") || svg_ieq(s, "transparent"))
        return 0;
    if (*s == '#') {
        int a, b, c, d, e, f;
        unsigned long n;

        s++;
        n = strlen(s);
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
        if (svg_ieq(s, named[i].name))
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
    s->pixels++;
    st = tstride(s->t);
    at = s->t->px + (long)y * st + (long)x;
    a = color >> 24;
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

#define SVG_POINT_MAX 256

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

static void svg_fill_polygon(struct svg_painter *s,
                             const int32_t *px, const int32_t *py,
                             int count, uint32_t color)
{
    int32_t ymin, ymax;
    int64_t y, yend;
    int i;

    if (!color || count < 3)
        return;
    ymin = ymax = py[0];
    for (i = 1; i < count; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    y = svg_max64(ymin, s->clip.y);
    yend = svg_min64(ymax,
                     (int64_t)s->clip.y + s->clip.h - 1);
    for (; y <= yend && s->work < SVG_WORK_MAX; y++) {
        int32_t cross[SVG_POINT_MAX];
        int nc = 0, a, b;

        for (i = 0; i < count && s->work < SVG_WORK_MAX; i++) {
            int j = i ? i - 1 : count - 1;
            int32_t y0 = py[j], y1 = py[i];

            s->work++;
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int64_t x = px[j] +
                    (y - (int64_t)y0) *
                    ((int64_t)px[i] - px[j]) /
                    ((int64_t)y1 - y0);

                if (nc < SVG_POINT_MAX)
                    cross[nc++] = (int32_t)x;
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

/* Flatten the common path commands into a bounded polyline. Cubic and
 * quadratic curves use fixed 12-segment subdivision. Elliptical arcs are
 * conservatively represented by their endpoint until a full arc solver is
 * added; malformed or excessively complex paths stop at the valid prefix. */
static int svg_path_points(const char *d, int32_t *x, int32_t *y,
                           int max, int *closed)
{
    int32_t cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    int n = 0, have_subpath = 0;

    *closed = 0;
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
            cx = sx; cy = sy;
            *closed = 1;
            break;
        }
        if (cmd == 'M' || cmd == 'm' || cmd == 'L' || cmd == 'l') {
            int32_t nx, ny;
            int was_move = cmd == 'M' || cmd == 'm';

            if (was_move && have_subpath)
                break;
            if (!svg_number(&d, &nx) || !svg_number(&d, &ny))
                break;
            if (relative) {
                nx = svg_sat_add(cx, nx);
                ny = svg_sat_add(cy, ny);
            }
            cx = nx; cy = ny;
            if (!n || was_move) {
                sx = cx; sy = cy;
                have_subpath = 1;
                cmd = relative ? 'l' : 'L';
            }
            x[n] = cx; y[n] = cy; n++;
        } else if (cmd == 'H' || cmd == 'h') {
            int32_t nx;
            if (!svg_number(&d, &nx)) break;
            cx = relative ? svg_sat_add(cx, nx) : nx;
            x[n] = cx; y[n] = cy; n++;
        } else if (cmd == 'V' || cmd == 'v') {
            int32_t ny;
            if (!svg_number(&d, &ny)) break;
            cy = relative ? svg_sat_add(cy, ny) : ny;
            x[n] = cx; y[n] = cy; n++;
        } else if (cmd == 'Q' || cmd == 'q') {
            int32_t qx, qy, nx, ny, ox = cx, oy = cy;
            int step;

            if (!svg_number(&d, &qx) || !svg_number(&d, &qy) ||
                !svg_number(&d, &nx) || !svg_number(&d, &ny))
                break;
            if (relative) {
                qx = svg_sat_add(ox, qx);
                qy = svg_sat_add(oy, qy);
                nx = svg_sat_add(ox, nx);
                ny = svg_sat_add(oy, ny);
            }
            for (step = 1; step <= 12 && n < max; step++) {
                int64_t a = 12 - step, b = step;
                x[n] = (int32_t)((a * a * ox + 2 * a * b * qx +
                                  b * b * nx) / 144);
                y[n] = (int32_t)((a * a * oy + 2 * a * b * qy +
                                  b * b * ny) / 144);
                n++;
            }
            cx = nx; cy = ny;
        } else if (cmd == 'C' || cmd == 'c') {
            int32_t x1, y1, x2, y2, nx, ny, ox = cx, oy = cy;
            int step;

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
            }
            cx = nx; cy = ny;
        } else if (cmd == 'A' || cmd == 'a') {
            int32_t discard, nx, ny;
            int k;

            for (k = 0; k < 5; k++)
                if (!svg_number(&d, &discard))
                    return n;
            if (!svg_number(&d, &nx) || !svg_number(&d, &ny))
                return n;
            if (relative) {
                nx = svg_sat_add(cx, nx);
                ny = svg_sat_add(cy, ny);
            }
            cx = nx; cy = ny;
            x[n] = cx; y[n] = cy; n++;
        } else {
            break;
        }
    }
    return n;
}

static int svg_paint_points_node(struct svg_painter *s,
                                 const struct dom_node *n,
                                 const struct svg_style *st,
                                 int32_t sw, int path)
{
    int32_t x[SVG_POINT_MAX], y[SVG_POINT_MAX];
    int count, close = 0, i;
    const char *source = dom_get_attr(n, path ? "d" : "points");

    count = path ? svg_path_points(source, x, y, SVG_POINT_MAX, &close)
                 : svg_points(source, x, y, SVG_POINT_MAX);
    if (!path && n->tag && svg_ieq(n->tag, "polygon"))
        close = 1;
    if (count < 2)
        return 0;
    for (i = 0; i < count; i++) {
        x[i] = svg_x(s, x[i]);
        y[i] = svg_y(s, y[i]);
    }
    /* SVG fills open subpaths as if closed. */
    if (st->fill && count >= 3)
        svg_fill_polygon(s, x, y, count, st->fill);
    svg_stroke_polyline(s, x, y, count, close, sw, st->stroke);
    return 1;
}

static void svg_style_for(const struct dom_node *n,
                          const struct svg_style *parent,
                          struct svg_style *out)
{
    const char *v;
    char value[80];

    *out = *parent;
    v = dom_get_attr(n, "fill");
    if (v) out->fill = svg_color(v, out->fill);
    v = dom_get_attr(n, "stroke");
    if (v) out->stroke = svg_color(v, out->stroke);
    out->stroke_width =
        svg_attr_num(n, "stroke-width", out->stroke_width);
    if (svg_style_value(n, "fill", value, sizeof(value)))
        out->fill = svg_color(value, out->fill);
    if (svg_style_value(n, "stroke", value, sizeof(value)))
        out->stroke = svg_color(value, out->stroke);
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
    svg_style_for(n, in, &st);
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

static int paint_inline_svg(const struct paint_target *t, lay_rect clip,
                            lay_rect dst, const struct dom_node *root)
{
    struct svg_painter s;
    struct svg_style st;
    const char *view, *preserve;
    int32_t v[4];
    int i;

    if (!root || root->tag_id != HTAG_SVG || dst.w <= 0 || dst.h <= 0)
        return 0;
    memset(&s, 0, sizeof(s));
    s.t = t;
    s.clip = lay_rect_intersect(clip, dst);
    s.dst = dst;
    s.minx = s.miny = 0;
    s.vieww = (int64_t)dst.w * 1000;
    s.viewh = (int64_t)dst.h * 1000;
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
    if (view && (!preserve || !svg_ieq(preserve, "none"))) {
        int64_t by_width = s.viewh * dst.w / s.vieww;
        int64_t by_height = s.vieww * dst.h / s.viewh;

        if ((int64_t)dst.w * s.viewh <=
            (int64_t)dst.h * s.vieww) {
            if (by_width < 1) by_width = 1;
            s.dst.y = svg_sat_coord((int64_t)dst.y +
                                    (dst.h - by_width) / 2);
            s.dst.h = (int32_t)by_width;
        } else {
            if (by_height < 1) by_height = 1;
            s.dst.x = svg_sat_coord((int64_t)dst.x +
                                    (dst.w - by_height) / 2);
            s.dst.w = (int32_t)by_height;
        }
    }
    st.fill = 0xFF000000u;
    st.stroke = 0;
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

/* ------------------------------------------------------------- images */

static void blit_image(const struct paint_target *t, lay_rect clip,
                       lay_rect dst, const struct image *im)
{
    int32_t x0, y0, x1, y1, x, y, st;
    lay_rect r;

    if (!im || im->w <= 0 || im->h <= 0 || dst.w <= 0 || dst.h <= 0)
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

            if (sx < 0) sx = 0;
            if (sx >= im->w) sx = im->w - 1;
            si = (long)sy * im->w + sx;
            px = im->px[si];
            if (im->has_alpha && im->alpha) {
                unsigned a = im->alpha[si];

                if (!a)
                    continue;
                row[x] = paint_blend(row[x], px, a);
            } else {
                row[x] = px & 0x00FFFFFFu;
            }
        }
    }
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
    const struct paint_target *t;
    const struct paint_opts *o;
    struct paint_stats *st;
    lay_rect dirty;
    int32_t dx, dy;      /* document -> surface translation */
};

static lay_rect xlate(const struct pstate *p, lay_rect r)
{
    r.x += p->dx;
    r.y += p->dy;
    return r;
}

static void paint_background(struct pstate *p, const struct lay_box *b)
{
    const struct computed_style *cs = b->style;
    lay_rect clip = lay_rect_intersect(xlate(p, lay_clip_rect(b)), p->dirty);
    lay_rect br = xlate(p, lay_border_rect(b));
    lay_rect pr = xlate(p, lay_padding_rect(b));

    if (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_LINE)
        return;
    if (b->flags & LAYF_HIDDEN)
        return;

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
    if (b->bord[CSS_TOP] || b->bord[CSS_RIGHT] || b->bord[CSS_BOTTOM] ||
        b->bord[CSS_LEFT]) {
        uint32_t col[4];
        uint8_t sty[4];
        int i;

        for (i = 0; i < 4; i++) {
            col[i] = cs->border_color[i];
            if (!CSS_COLOR_A(col[i]))
                col[i] = cs->color;
            sty[i] = cs->border_style[i];
        }
        paint_frame(p->t, clip, br, b->bord[CSS_TOP], b->bord[CSS_RIGHT],
                    b->bord[CSS_BOTTOM], b->bord[CSS_LEFT], col, sty);
        if (p->st) p->st->rects++;
    }
    if (p->o->debug_boxes) {
        uint32_t col[4];
        uint8_t sty[4];
        int i;

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

static void paint_content(struct pstate *p, const struct lay_box *b)
{
    const struct computed_style *cs = b->style;
    lay_rect clip = lay_rect_intersect(xlate(p, lay_clip_rect(b)), p->dirty);
    font_surface fs;
    font_rc frc;
    uint32_t fg;

    if (b->flags & LAYF_HIDDEN)
        return;
    fg = CSS_COLOR_RGB(cs->color);
    if ((b->flags & LAYF_LINK) && p->o->link_color)
        fg = CSS_COLOR_RGB(p->o->link_color);

    fs = font_surface_of(p->t->px, p->t->w, p->t->h, tstride(p->t));
    frc = font_mkrc(clip.x, clip.y, clip.w, clip.h);
    if (frc.w <= 0 || frc.h <= 0)
        return;

    switch (b->kind) {
    case LAY_BOX_REPLACED: {
        lay_rect cr = xlate(p, lay_content_rect(b));
        const struct image *im = (b->src && p->o->image)
            ? p->o->image(p->o->image_ctx, b->src) : 0;

        if (paint_form_control(p, b, clip, cr, fg)) {
            if (p->st) p->st->rects++;
        } else if (im) {
            blit_image(p->t, clip, cr, im);
            if (p->st) p->st->images++;
        } else if (b->node && b->node->tag_id == HTAG_SVG &&
                   paint_inline_svg(p->t, clip, cr, b->node)) {
            if (p->st) p->st->images++;
        } else {
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
        lay_rect r = xlate(p, lay_mkrect(b->x, b->y, b->w, b->h));

        if (b->marker)
            font_draw_text_n(&fs, frc, r.x, r.y, b->marker, -1,
                             b->font ? b->font : lay_font_for(cs), fg,
                             FONT_TRANSPARENT);
        else
            draw_marker_glyph(p->t, clip, b, p->dx, p->dy,
                              0xFF000000u | fg);
        if (p->st) p->st->glyphs++;
        return;
    }
    case LAY_BOX_TEXT: {
        lay_rect r = xlate(p, lay_mkrect(b->x, b->y, b->w, b->h));
        uint32_t s0 = 0, s1 = 0, sfg = 0, sbg = 0;
        int sel = 0;

        if (!b->font || !b->text_len)
            return;
        if (p->o->selection)
            sel = p->o->selection(p->o->selection_ctx, b, &s0, &s1,
                                  &sfg, &sbg);
        if (sel) {
            if (s1 > b->text_len) s1 = b->text_len;
            if (s0 > s1) s0 = s1;
        }
        if (sel && s1 > s0) {
            int32_t x0 = r.x + (int32_t)s0 * b->font->w;
            int32_t x1 = r.x + (int32_t)s1 * b->font->w;

            paint_fill(p->t, clip, lay_mkrect(x0, r.y, x1 - x0, r.h),
                       CSS_COLOR_A(sbg) ? sbg : 0xFF5BA3D0u);
            font_draw_text_n(&fs, frc, r.x, r.y, b->text, (int)s0, b->font,
                             fg, FONT_TRANSPARENT);
            font_draw_text_n(&fs, frc, x0, r.y, b->text + s0,
                             (int)(s1 - s0), b->font,
                             CSS_COLOR_A(sfg) ? CSS_COLOR_RGB(sfg) : 0xFFFFFF,
                             FONT_TRANSPARENT);
            font_draw_text_n(&fs, frc, x1, r.y, b->text + s1,
                             (int)(b->text_len - s1), b->font, fg,
                             FONT_TRANSPARENT);
        } else {
            font_draw_text_n(&fs, frc, r.x, r.y, b->text, (int)b->text_len,
                             b->font, fg, FONT_TRANSPARENT);
        }
        if (p->st) p->st->glyphs += b->text_len;
        if (cs->text_decoration)
            draw_decorations(p->t, clip, b, r.x, r.y, b->w,
                             0xFF000000u | fg);
        if (p->o->caret_box == b) {
            int32_t cx = r.x + (int32_t)(p->o->caret_offset > b->text_len
                                         ? b->text_len : p->o->caret_offset) *
                         b->font->w;

            paint_fill(p->t, clip, lay_mkrect(cx, r.y, 1, r.h),
                       p->o->caret_color ? p->o->caret_color : 0xFF000000u);
        }
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
        lay_rect ink = xlate(&p, lay_ink_rect(b));
        lay_rect own;

        if (stats)
            stats->items_considered++;
        /* Two O(1) rejections: the subtree ink extent, then the box. */
        if (!lay_rect_intersects(ink, dirty))
            continue;
        own = (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_MARKER)
            ? xlate(&p, lay_mkrect(b->x, b->y, b->w, b->h))
            : xlate(&p, lay_border_rect(b));
        if (!lay_rect_intersects(own, dirty))
            continue;
        if (!lay_rect_intersects(xlate(&p, lay_clip_rect(b)), dirty))
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
        lay_rect r = xlate(&p, lay_border_rect(o->highlight_box));
        uint32_t col[4];
        uint8_t sty[4];
        int k;

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
