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

        if (im) {
            blit_image(p->t, clip, cr, im);
            if (p->st) p->st->images++;
        } else {
            /* The placeholder layout reserved: a hairline box with the
             * alt text in it, so a page with broken images still reads. */
            uint32_t col[4];
            uint8_t sty[4];
            const char *alt = b->node ? dom_get_attr(b->node, "alt") : 0;
            int i;

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
