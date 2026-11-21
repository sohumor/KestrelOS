/* layout.c - the CSS 2.1 visual formatting model.
 *
 * See layout.h for what is implemented. The order of this file follows
 * the order of the algorithm:
 *
 *   1  arena, small helpers, length resolution
 *   2  font selection and text measurement
 *   3  white-space processing and text folding
 *   4  box tree construction and anonymous boxes
 *   5  intrinsic (min/max content) widths
 *   6  floats
 *   7  inline formatting: line boxes
 *   8  block formatting: stacking and margin collapsing
 *   9  tables
 *  10  out-of-flow: absolute, fixed, relative
 *  11  post pass: clips, ink extents, paint order, node index
 *  12  the public entry points and hit testing
 *
 * Only <stdlib.h> and <string.h> are used, so this builds against the
 * host libc and against the KestrelOS freestanding libc unchanged.
 */

#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "css.h"
#include "font.h"
#include "layout.h"
#include "layout_arena.h"

/* ================================================================== *
 * 1  arena, helpers, lengths
 * ================================================================== */

#define LAY_CHUNK_MIN (64UL * 1024UL)

static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t clamp32(long long v)
{
    if (v < -(1LL << 29)) return -(int32_t)(1 << 29);
    if (v >  (1LL << 29)) return  (int32_t)(1 << 29);
    return (int32_t)v;
}

void *lay_arena_alloc(struct lay_document *L, unsigned long n)
{
    struct lay_chunk *c;
    unsigned long want, hdr;
    char *p;

    n = (n + 15UL) & ~15UL;
    hdr = (sizeof(struct lay_chunk) + 15UL) & ~15UL;

    c = L->chunks;
    if (c && c->cap - c->used >= n) {
        p = (char *)c + hdr + c->used;
        c->used += n;
        return p;
    }
    want = n + hdr;
    if (want < LAY_CHUNK_MIN)
        want = LAY_CHUNK_MIN;
    if (L->arena_bytes + want > LAY_MAX_ARENA) {
        L->trunc |= LAY_TRUNC_MEMORY;
        return 0;
    }
    c = (struct lay_chunk *)malloc(want);
    if (!c) {
        L->trunc |= LAY_TRUNC_MEMORY;
        return 0;
    }
    c->next = L->chunks;
    c->cap = want - hdr;
    c->used = n;
    L->chunks = c;
    L->arena_bytes += want;
    return (char *)c + hdr;
}

char *lay_arena_str(struct lay_document *L, const char *s, unsigned long n)
{
    char *p = (char *)lay_arena_alloc(L, n + 1);

    if (!p)
        return 0;
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

lay_rect lay_rect_intersect(lay_rect a, lay_rect b)
{
    int32_t x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int32_t x1 = imin(a.x + a.w, b.x + b.w);
    int32_t y1 = imin(a.y + a.h, b.y + b.h);

    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return lay_mkrect(x0, y0, x1 - x0, y1 - y0);
}

lay_rect lay_rect_union(lay_rect a, lay_rect b)
{
    int32_t x0, y0, x1, y1;

    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    x0 = imin(a.x, b.x);
    y0 = imin(a.y, b.y);
    x1 = imax(a.x + a.w, b.x + b.w);
    y1 = imax(a.y + a.h, b.y + b.h);
    return lay_mkrect(x0, y0, x1 - x0, y1 - y0);
}

const char *lay_kind_name(int kind)
{
    static const char *const n[LAY_BOX_KIND_COUNT] = {
        "block", "inline", "text", "line", "replaced", "marker",
        "table", "rowgroup", "row", "cell"
    };

    if (kind < 0 || kind >= LAY_BOX_KIND_COUNT)
        return "?";
    return n[kind];
}

/* ---- lengths ----------------------------------------------------- *
 *
 * `base` < 0 means the percentage base is indefinite (an auto-height
 * containing block), in which case a percentage computes to `dflt`,
 * which is what CSS 2.1 10.5 asks for.
 */
static int32_t used_len(struct css_len l, int32_t base, int32_t dflt)
{
    switch (l.type) {
    case CSS_LEN_PX:
        return l.v;
    case CSS_LEN_PCT:
        if (base < 0)
            return dflt;
        return clamp32((long long)base * l.v / 100000);
    case CSS_LEN_CALC:
        if (base < 0)
            return l.pct ? dflt : clamp32((long long)l.v);
        return clamp32(((int64_t)base * l.pct) / 100000 + (int64_t)l.v);
    case CSS_LEN_AUTO:
    case CSS_LEN_NONE:
    case CSS_LEN_NORMAL:
    case CSS_LEN_NUMBER:
    default:
        return dflt;
    }
}

static int len_is_auto(struct css_len l)
{
    return l.type == CSS_LEN_AUTO;
}

static int len_is_auto_or_indef(struct css_len l, int32_t base)
{
    if (l.type == CSS_LEN_AUTO || l.type == CSS_LEN_NONE)
        return 1;
    if (base < 0) {
        if (l.type == CSS_LEN_PCT)
            return 1;
        if (l.type == CSS_LEN_CALC && l.pct != 0)
            return 1;
    }
    return 0;
}

static int32_t box_h_bp(const struct lay_box *b)
{
    return b->bord[CSS_LEFT] + b->pad[CSS_LEFT] +
           b->pad[CSS_RIGHT] + b->bord[CSS_RIGHT];
}

static int32_t box_v_bp(const struct lay_box *b)
{
    return b->bord[CSS_TOP] + b->pad[CSS_TOP] +
           b->pad[CSS_BOTTOM] + b->bord[CSS_BOTTOM];
}

static int32_t content_dim(int32_t v, const struct computed_style *cs,
                           int32_t bp)
{
    if (cs->box_sizing == CSS_BOXSIZING_BORDER)
        v -= bp;
    return v > 0 ? v : 0;
}

static int32_t vedge_bp(const struct computed_style *cs)
{
    return used_len(cs->padding[CSS_TOP], -1, 0) +
           used_len(cs->padding[CSS_BOTTOM], -1, 0) +
           cs->border_width[CSS_TOP] + cs->border_width[CSS_BOTTOM];
}

static int32_t ratio_mul(int32_t v, int64_t num, int64_t den)
{
    if (v <= 0 || num <= 0 || den <= 0)
        return 0;
    return clamp32(((int64_t)v * num + den / 2) / den);
}

static int32_t ratio_height_from_width(const struct computed_style *cs,
                                       int32_t inner_w, int32_t h_bp,
                                       int32_t v_bp)
{
    if (!cs || cs->aspect_ratio <= 0)
        return 0;
    if (cs->box_sizing == CSS_BOXSIZING_BORDER) {
        int32_t bw = imax(0, inner_w + h_bp);
        int32_t bh = ratio_mul(bw, 1000LL, cs->aspect_ratio);
        return imax(0, bh - v_bp);
    }
    return ratio_mul(imax(0, inner_w), 1000LL, cs->aspect_ratio);
}

static int32_t ratio_width_from_height(const struct computed_style *cs,
                                       int32_t inner_h, int32_t h_bp,
                                       int32_t v_bp)
{
    if (!cs || cs->aspect_ratio <= 0)
        return 0;
    if (cs->box_sizing == CSS_BOXSIZING_BORDER) {
        int32_t bh = imax(0, inner_h + v_bp);
        int32_t bw = ratio_mul(bh, cs->aspect_ratio, 1000LL);
        return imax(0, bw - h_bp);
    }
    return ratio_mul(imax(0, inner_h), cs->aspect_ratio, 1000LL);
}

/* min/max clamping. max-width: none is CSS_LEN_NONE. */
static int32_t clamp_width(int32_t w, const struct computed_style *cs,
                           int32_t base, int32_t h_bp)
{
    int32_t mx, mn;
    int32_t sub = cs->box_sizing == CSS_BOXSIZING_BORDER ? h_bp : 0;

    if (cs->max_width.type != CSS_LEN_NONE &&
        cs->max_width.type != CSS_LEN_AUTO &&
        !(cs->max_width.type == CSS_LEN_PCT && base < 0) &&
        !(cs->max_width.type == CSS_LEN_CALC && base < 0 && cs->max_width.pct)) {
        mx = used_len(cs->max_width, base, 0x3FFFFFFF);
        mx = imax(0, mx - sub);
        if (w > mx)
            w = mx;
    }
    if (!(cs->min_width.type == CSS_LEN_PCT && base < 0) &&
        !(cs->min_width.type == CSS_LEN_CALC && base < 0 && cs->min_width.pct)) {
        mn = used_len(cs->min_width, base, 0);
        if (cs->min_width.type != CSS_LEN_AUTO)
            mn = imax(0, mn - sub);
        if (w < mn)
            w = mn;
    }
    if (w < 0)
        w = 0;
    return w;
}

static int32_t clamp_height(int32_t h, const struct computed_style *cs,
                            int32_t base, int32_t v_bp)
{
    int32_t mx, mn;
    int32_t sub = cs->box_sizing == CSS_BOXSIZING_BORDER ? v_bp : 0;

    if (cs->max_height.type != CSS_LEN_NONE &&
        cs->max_height.type != CSS_LEN_AUTO &&
        !(cs->max_height.type == CSS_LEN_PCT && base < 0) &&
        !(cs->max_height.type == CSS_LEN_CALC && base < 0 && cs->max_height.pct)) {
        mx = used_len(cs->max_height, base, 0x3FFFFFFF);
        mx = imax(0, mx - sub);
        if (h > mx)
            h = mx;
    }
    if (!(cs->min_height.type == CSS_LEN_PCT && base < 0) &&
        !(cs->min_height.type == CSS_LEN_CALC && base < 0 && cs->min_height.pct)) {
        mn = used_len(cs->min_height, base, 0);
        if (cs->min_height.type != CSS_LEN_AUTO)
            mn = imax(0, mn - sub);
        if (h < mn)
            h = mn;
    }
    if (h < 0)
        h = 0;
    return h;
}

/* ---- collapsible margins ----------------------------------------- */

static struct lay_marg marg_of(int32_t v)
{
    struct lay_marg m;

    m.pos = v > 0 ? v : 0;
    m.neg = v < 0 ? v : 0;
    return m;
}

static struct lay_marg marg_collapse(struct lay_marg a, struct lay_marg b)
{
    struct lay_marg m;

    m.pos = a.pos > b.pos ? a.pos : b.pos;
    m.neg = a.neg < b.neg ? a.neg : b.neg;
    return m;
}

static int32_t marg_value(struct lay_marg m)
{
    return m.pos + m.neg;
}

/* ================================================================== *
 * 2  fonts
 * ================================================================== */

/* The four faces have line heights 12, 16, 24 and 32 px. CSS font sizes
 * are em sizes, so the thresholds are chosen to put the UA stylesheet's
 * headings on the faces a reader expects: h1 (2em = 32px) on 16x32,
 * h2 (1.5em = 24px) on 12x24, h3 (1.17em ~ 19px) and body text on 8x16,
 * <small> (0.83em ~ 13px) on 6x12. */
int lay_font_size_class(int32_t px)
{
    if (px <= 13) return FONT_SMALL;
    if (px <= 20) return FONT_BODY;
    if (px <= 28) return FONT_LARGE;
    return FONT_HUGE;
}

const struct font *lay_font_for(const struct computed_style *cs)
{
    int w, s;

    if (!cs)
        return font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN);
    w = cs->font_weight >= 600 ? FONT_BOLD : FONT_REGULAR;
    s = (cs->font_style == CSS_FONTSTYLE_ITALIC ||
         cs->font_style == CSS_FONTSTYLE_OBLIQUE) ? FONT_ITALIC : FONT_ROMAN;
    return font_get(lay_font_size_class(cs->font_size), w, s);
}

int32_t lay_line_height(const struct computed_style *cs)
{
    const struct font *f;
    int32_t v;

    f = lay_font_for(cs);
    switch (cs->line_height.type) {
    case CSS_LEN_PX:
        v = cs->line_height.v;
        break;
    case CSS_LEN_NUMBER:
        v = clamp32((long long)cs->font_size * cs->line_height.v / 1000);
        break;
    case CSS_LEN_PCT:
        v = clamp32((long long)cs->font_size * cs->line_height.v / 100000);
        break;
    case CSS_LEN_CALC:
        v = clamp32((long long)cs->font_size * cs->line_height.pct / 100000 +
                    (long long)cs->line_height.v);
        break;
    case CSS_LEN_NORMAL:
    default:
        v = font_line_height(f);
        break;
    }
    if (v < 0)
        v = 0;
    return v;
}

/* Half-leading split of a line-height over a face. */
static void half_leading(const struct computed_style *cs, const struct font *f,
                         int32_t *asc, int32_t *desc)
{
    int32_t lh = lay_line_height(cs);
    int32_t lead = lh - font_line_height(f);
    int32_t top = lead >= 0 ? lead / 2 : -((-lead + 1) / 2);

    *asc = font_ascent(f) + top;
    *desc = lh - *asc;
}

static int32_t text_width(const struct font *f, const char *s, unsigned long n)
{
    /* Every face is monospaced, so this is exact and O(1); the call to
     * font_text_width_n stays as the definition of record. */
    (void)s;
    return (int32_t)((long)f->w * (long)n);
}

static int32_t text_char_step(const struct font *f,
                              const struct computed_style *cs)
{
    int32_t ls = cs ? cs->letter_spacing : 0;
    int32_t step = f->w + ls;
    return step > 0 ? step : 0;
}

static int32_t text_width_adv(const struct font *f,
                              const struct computed_style *cs, unsigned long n)
{
    return (int32_t)((long)text_char_step(f, cs) * (long)n);
}

static int32_t text_width_vis(const struct font *f,
                              const struct computed_style *cs, unsigned long n)
{
    int32_t step = text_char_step(f, cs);
    if (n == 0)
        return 0;
    return (int32_t)((long)(n - 1) * (long)step + (long)f->w);
}

/* ================================================================== *
 * 3  white-space processing and text folding
 * ================================================================== */

static int ws_is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int ws_collapses(int white_space)
{
    return white_space == CSS_WHITESPACE_NORMAL ||
           white_space == CSS_WHITESPACE_NOWRAP ||
           white_space == CSS_WHITESPACE_PRE_LINE;
}

static int ws_wraps(int white_space)
{
    return white_space == CSS_WHITESPACE_NORMAL ||
           white_space == CSS_WHITESPACE_PRE_WRAP ||
           white_space == CSS_WHITESPACE_PRE_LINE;
}

static int ws_keeps_newline(int white_space)
{
    return white_space == CSS_WHITESPACE_PRE ||
           white_space == CSS_WHITESPACE_PRE_WRAP ||
           white_space == CSS_WHITESPACE_PRE_LINE;
}

static int up_ascii(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

static int lo_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Fold a DOM text node's UTF-8 to the ASCII the bitmap faces can draw and
 * apply text-transform. Returns an arena string plus its length; the
 * returned buffer is what layout measures and what paint draws, so the
 * two can never disagree about a width. */
static const char *fold_text(struct lay_document *L, const char *s,
                             unsigned long n, int transform,
                             unsigned long *out_len)
{
    unsigned long want, i;
    char *p;
    int prev_alpha;

    *out_len = 0;
    if (!s || !n)
        return "";
    if (n > DOM_MAX_TEXT)
        n = DOM_MAX_TEXT;
    want = dom_fold_ascii(s, n, 0, 0);
    if (want > DOM_MAX_TEXT)
        want = DOM_MAX_TEXT;
    p = (char *)lay_arena_alloc(L, want + 1);
    if (!p)
        return "";
    dom_fold_ascii(s, n, p, want + 1);
    p[want] = 0;

    prev_alpha = 0;
    for (i = 0; i < want; i++) {
        int c = (unsigned char)p[i];

        switch (transform) {
        case CSS_TEXTTRANSFORM_UPPERCASE:
            p[i] = (char)up_ascii(c);
            break;
        case CSS_TEXTTRANSFORM_LOWERCASE:
            p[i] = (char)lo_ascii(c);
            break;
        case CSS_TEXTTRANSFORM_CAPITALIZE:
            if (!prev_alpha)
                p[i] = (char)up_ascii(c);
            break;
        default:
            break;
        }
        c = (unsigned char)p[i];
        prev_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9');
    }
    *out_len = want;
    return p;
}

/* ================================================================== *
 * 4  box tree construction
 * ================================================================== */

static struct computed_style *lay_initial_style(struct lay_document *L,
                                                int block)
{
    struct computed_style *cs =
        (struct computed_style *)lay_arena_alloc(L, sizeof *cs);

    if (!cs)
        return 0;
    css_style_initial(cs);
    cs->display = block ? CSS_DISPLAY_BLOCK : CSS_DISPLAY_INLINE;
    if (L->opt.default_font_size > 0)
        cs->font_size = L->opt.default_font_size;
    return cs;
}

/* Every box must have a style. Elements the cascade did not reach (a
 * document that was never styled, or a node created after the cascade
 * ran) fall back to the initial style, blockified from the tag table so
 * that an unstyled document is still readable rather than one long
 * inline run. */
static const struct computed_style *style_of(struct lay_document *L,
                                             struct dom_node *n)
{
    if (n && n->style)
        return (const struct computed_style *)n->style;
    if (n && n->type == DOM_ELEMENT)
        return lay_initial_style(L, (dom_tag_flags(n->tag_id) & DTF_BLOCK) != 0);
    return lay_initial_style(L, 0);
}

static struct lay_box *box_new(struct lay_ctx *c, int kind,
                               const struct computed_style *cs,
                               struct dom_node *n)
{
    struct lay_box *b;

    if (c->L->nboxes >= c->L->opt.max_boxes) {
        c->L->trunc |= LAY_TRUNC_BOXES;
        return 0;
    }
    b = (struct lay_box *)lay_arena_alloc(c->L, sizeof *b);
    if (!b)
        return 0;
    memset(b, 0, sizeof *b);
    b->kind = (uint8_t)kind;
    b->style = cs;
    b->node = n;
    b->order = c->order++;
    b->colspan = 1;
    b->rowspan = 1;
    b->valign = cs ? cs->vertical_align : CSS_VALIGN_BASELINE;
    c->L->nboxes++;
    return b;
}

static void box_append(struct lay_box *p, struct lay_box *ch)
{
    ch->parent = p;
    ch->prev = p->last_child;
    ch->next = 0;
    if (p->last_child)
        p->last_child->next = ch;
    else
        p->first_child = ch;
    p->last_child = ch;
    ch->depth = (uint8_t)(p->depth + 1 > 255 ? 255 : p->depth + 1);
}

static void box_unlink(struct lay_box *ch)
{
    struct lay_box *p = ch->parent;

    if (!p)
        return;
    if (ch->prev) ch->prev->next = ch->next; else p->first_child = ch->next;
    if (ch->next) ch->next->prev = ch->prev; else p->last_child = ch->prev;
    ch->parent = 0;
    ch->next = ch->prev = 0;
}

/* ---- classification ---------------------------------------------- */

static int disp_is_none(int d) { return d == CSS_DISPLAY_NONE; }

/* CSS 2.1 9.7: floating or absolutely positioning an element blockifies
 * its display. */
static int used_display(const struct computed_style *cs)
{
    int d = cs->display;

    if (d == CSS_DISPLAY_NONE)
        return d;
    if (cs->position == CSS_POSITION_ABSOLUTE ||
        cs->position == CSS_POSITION_FIXED ||
        cs->css_float != CSS_FLOAT_NONE) {
        switch (d) {
        case CSS_DISPLAY_INLINE:
        case CSS_DISPLAY_INLINE_BLOCK:
        case CSS_DISPLAY_LIST_ITEM:
            return d == CSS_DISPLAY_LIST_ITEM ? CSS_DISPLAY_LIST_ITEM
                                              : CSS_DISPLAY_BLOCK;
        case CSS_DISPLAY_INLINE_TABLE:
            return CSS_DISPLAY_TABLE;
        case CSS_DISPLAY_INLINE_FLEX:
        case CSS_DISPLAY_FLEX:
            return CSS_DISPLAY_FLEX;
        case CSS_DISPLAY_INLINE_GRID:
        case CSS_DISPLAY_GRID:
            return CSS_DISPLAY_GRID;
        default:
            return d;
        }
    }
    return d;
}

/* Flex containers use the bounded row layout below.  Unsupported column
 * flex and grid constructs retain a readable block (or inline-block)
 * fallback instead of dropping their contents. */
static int box_kind_for_display(int d, uint32_t *flags)
{
    switch (d) {
    case CSS_DISPLAY_INLINE:
        return LAY_BOX_INLINE;
    case CSS_DISPLAY_BLOCK:
    case CSS_DISPLAY_LIST_ITEM:
    case CSS_DISPLAY_FLEX:
    case CSS_DISPLAY_GRID:
    case CSS_DISPLAY_TABLE_CAPTION:
        return LAY_BOX_BLOCK;
    case CSS_DISPLAY_INLINE_BLOCK:
    case CSS_DISPLAY_INLINE_FLEX:
    case CSS_DISPLAY_INLINE_GRID:
        *flags |= LAYF_ATOMIC | LAYF_BFC;
        return LAY_BOX_BLOCK;
    case CSS_DISPLAY_TABLE:
        return LAY_BOX_TABLE;
    case CSS_DISPLAY_INLINE_TABLE:
        *flags |= LAYF_ATOMIC | LAYF_BFC;
        return LAY_BOX_TABLE;
    case CSS_DISPLAY_TABLE_ROW_GROUP:
    case CSS_DISPLAY_TABLE_HEADER_GROUP:
    case CSS_DISPLAY_TABLE_FOOTER_GROUP:
        return LAY_BOX_TABLE_ROWGROUP;
    case CSS_DISPLAY_TABLE_ROW:
        return LAY_BOX_TABLE_ROW;
    case CSS_DISPLAY_TABLE_CELL:
        *flags |= LAYF_BFC;
        return LAY_BOX_TABLE_CELL;
    default:
        return LAY_BOX_BLOCK;
    }
}

static int box_is_inline_level(const struct lay_box *b)
{
    if (b->flags & (LAYF_FLOAT | LAYF_ABSOLUTE))
        return 0;
    if (b->kind == LAY_BOX_INLINE || b->kind == LAY_BOX_TEXT ||
        b->kind == LAY_BOX_REPLACED || b->kind == LAY_BOX_MARKER)
        return 1;
    return (b->flags & LAYF_ATOMIC) != 0;
}

static int box_is_out_of_flow(const struct lay_box *b)
{
    return (b->flags & (LAYF_FLOAT | LAYF_ABSOLUTE)) != 0;
}

/* ---- list markers ------------------------------------------------ */

static void roman(int32_t n, int upper, char *out, unsigned cap)
{
    static const int val[13] = {1000, 900, 500, 400, 100, 90, 50, 40,
                                10, 9, 5, 4, 1};
    static const char *const sym[13] = {"m", "cm", "d", "cd", "c", "xc",
                                        "l", "xl", "x", "ix", "v", "iv", "i"};
    unsigned o = 0;
    int i;

    if (n < 1 || n > 3999) {
        /* Outside the range roman numerals express, fall back to digits
         * rather than emitting nonsense. */
        int neg = n < 0;
        char tmp[16];
        int t = 0;
        long v = neg ? -(long)n : (long)n;

        if (!v) tmp[t++] = '0';
        while (v && t < 15) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        if (neg && o + 1 < cap) out[o++] = '-';
        while (t-- > 0 && o + 1 < cap) out[o++] = tmp[t];
        out[o] = 0;
        return;
    }
    for (i = 0; i < 13 && o + 3 < cap; i++) {
        while (n >= val[i]) {
            const char *s = sym[i];

            while (*s && o + 1 < cap)
                out[o++] = upper ? (char)up_ascii((unsigned char)*s++)
                                 : *s++;
            n -= val[i];
        }
    }
    out[o] = 0;
}

static void alpha_num(int32_t n, int upper, char *out, unsigned cap)
{
    char tmp[16];
    int t = 0;
    unsigned o = 0;

    if (n < 1) {
        /* 0 and negatives have no alphabetic form; use digits. */
        long v = n;
        int neg = v < 0;

        if (neg) v = -v;
        if (!v) tmp[t++] = '0';
        while (v && t < 15) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        if (neg && o + 1 < cap) out[o++] = '-';
        while (t-- > 0 && o + 1 < cap) out[o++] = tmp[t];
        out[o] = 0;
        return;
    }
    while (n > 0 && t < 15) {
        int r = (n - 1) % 26;

        tmp[t++] = (char)((upper ? 'A' : 'a') + r);
        n = (n - 1) / 26;
    }
    while (t-- > 0 && o + 1 < cap)
        out[o++] = tmp[t];
    out[o] = 0;
}

static void decimal_num(int32_t n, int pad2, char *out, unsigned cap)
{
    char tmp[16];
    int t = 0;
    unsigned o = 0;
    long v = n;
    int neg = v < 0;

    if (neg) v = -v;
    if (!v) tmp[t++] = '0';
    while (v && t < 15) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    if (pad2 && t < 2 && t < 15) tmp[t++] = '0';
    if (neg && o + 1 < cap) out[o++] = '-';
    while (t-- > 0 && o + 1 < cap) out[o++] = tmp[t];
    out[o] = 0;
}

/* Ordinal of a list item: the count of preceding siblings that are list
 * items, offset by <ol start> and reset by <li value>. */
static int32_t list_ordinal(struct dom_node *li)
{
    struct dom_node *p, *s;
    const char *a;
    int32_t next = 1;

    if (!li)
        return 1;
    a = dom_get_attr(li, "value");
    if (a && *a)
        return (int32_t)atol(a);
    p = li->parent;
    if (p) {
        a = dom_get_attr(p, "start");
        if (a && *a)
            next = (int32_t)atol(a);
    }
    for (s = p ? p->first_child : 0; s && s != li; s = s->next_sibling) {
        const struct computed_style *cs;
        int is_item;

        if (s->type != DOM_ELEMENT)
            continue;
        cs = (const struct computed_style *)s->style;
        is_item = cs ? cs->display == CSS_DISPLAY_LIST_ITEM
                     : s->tag_id == HTAG_LI;
        if (!is_item)
            continue;
        a = dom_get_attr(s, "value");
        if (a && *a)
            next = (int32_t)atol(a);
        if (next < 0x7fffffff)
            next++;
    }
    return next;
}

/* Marker text, or 0 when the marker is a glyph the painter draws (disc,
 * circle, square). Trailing space is included so the measured width is
 * the width the marker actually occupies. */
static const char *marker_text(struct lay_document *L,
                               const struct computed_style *cs,
                               struct dom_node *li)
{
    char buf[40];
    int32_t n;
    unsigned len;

    switch (cs->list_style_type) {
    case CSS_LISTSTYLE_NONE:
    case CSS_LISTSTYLE_DISC:
    case CSS_LISTSTYLE_CIRCLE:
    case CSS_LISTSTYLE_SQUARE:
        return 0;
    default:
        break;
    }
    n = list_ordinal(li);
    switch (cs->list_style_type) {
    case CSS_LISTSTYLE_DECIMAL:
        decimal_num(n, 0, buf, sizeof buf);
        break;
    case CSS_LISTSTYLE_DECIMAL_LEADING_ZERO:
        decimal_num(n, 1, buf, sizeof buf);
        break;
    case CSS_LISTSTYLE_LOWER_ROMAN:
        roman(n, 0, buf, sizeof buf);
        break;
    case CSS_LISTSTYLE_UPPER_ROMAN:
        roman(n, 1, buf, sizeof buf);
        break;
    case CSS_LISTSTYLE_UPPER_ALPHA:
    case CSS_LISTSTYLE_UPPER_LATIN:
        alpha_num(n, 1, buf, sizeof buf);
        break;
    default:
        alpha_num(n, 0, buf, sizeof buf);
        break;
    }
    len = (unsigned)strlen(buf);
    if (len + 2 < sizeof buf) {
        buf[len++] = '.';
        buf[len] = 0;
    }
    return lay_arena_str(L, buf, len);
}

/* ---- replaced elements ------------------------------------------- */

static int attr_px(struct dom_node *n, const char *name, int32_t *out)
{
    const char *a = dom_get_attr(n, name);
    long v;

    if (!a || !*a)
        return 0;
    v = atol(a);
    if (v <= 0 || v > 100000)
        return 0;
    *out = (int32_t)v;
    return 1;
}

static int node_is_replaced(struct dom_node *n)
{
    if (!n || n->type != DOM_ELEMENT)
        return 0;
    switch (n->tag_id) {
    case HTAG_IMG:
    case HTAG_AUDIO:
    case HTAG_CANVAS:
    case HTAG_VIDEO:
    case HTAG_SVG:
    case HTAG_EMBED:
    case HTAG_OBJECT:
    case HTAG_INPUT:
    case HTAG_SELECT:
    case HTAG_TEXTAREA:
        return 1;
    default:
        return 0;
    }
}

static int attr_positive(struct dom_node *n, const char *name, int def,
                         int max)
{
    const char *s = dom_get_attr(n, name);
    long v = s && *s ? atol(s) : def;

    if (v <= 0)
        v = def;
    if (v > max)
        v = max;
    return (int)v;
}

static int value_is(const char *a, const char *b)
{
    if (!a)
        return 0;
    while (*a && *b) {
        if (lo_ascii((unsigned char)*a) != lo_ascii((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int viewbox_number(const char **pp, int64_t *out)
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
    *out = sign * (whole * 1000 + frac * 1000 / scale);
    *pp = p;
    return 1;
}

static int svg_viewbox_ratio(struct dom_node *n, int64_t *vw, int64_t *vh)
{
    const char *p = dom_get_attr(n, "viewbox");
    int64_t v[4];
    int i;

    if (!p)
        return 0;
    for (i = 0; i < 4; i++)
        if (!viewbox_number(&p, &v[i]))
            return 0;
    if (v[2] <= 0 || v[3] <= 0)
        return 0;
    *vw = v[2];
    *vh = v[3];
    return 1;
}

static void control_intrinsic(struct dom_node *n, int32_t *w, int32_t *h)
{
    const char *type;

    if (n->tag_id == HTAG_AUDIO) {
        if (!*w) *w = 300;
        if (!*h) *h = 32;
        return;
    }
    if (n->tag_id == HTAG_SVG) {
        int64_t vw, vh;

        if (svg_viewbox_ratio(n, &vw, &vh)) {
            if (*w && !*h) {
                int64_t derived = (int64_t)*w * vh / vw;
                *h = (int32_t)(derived < 1 ? 1 :
                               (derived > 100000 ? 100000 : derived));
            } else if (*h && !*w) {
                int64_t derived = (int64_t)*h * vw / vh;
                *w = (int32_t)(derived < 1 ? 1 :
                               (derived > 100000 ? 100000 : derived));
            }
        }
        if (!*w) *w = 300;
        if (!*h) *h = 150;
        return;
    }
    if (n->tag_id == HTAG_VIDEO || n->tag_id == HTAG_CANVAS) {
        if (!*w) *w = 300;
        if (!*h) *h = 150;
        return;
    }
    if (n->tag_id == HTAG_TEXTAREA) {
        *w = attr_positive(n, "cols", 20, 200) * 8 + 8;
        *h = attr_positive(n, "rows", 2, 100) * 16 + 8;
        return;
    }
    if (n->tag_id == HTAG_SELECT) {
        int rows = dom_has_attr(n, "multiple")
            ? attr_positive(n, "size", 4, 100) : 1;

        *w = 180;
        *h = rows * 18 + 6;
        return;
    }
    if (n->tag_id != HTAG_INPUT)
        return;
    type = dom_get_attr(n, "type");
    if (!type || !*type)
        type = "text";
    if (value_is(type, "checkbox") || value_is(type, "radio")) {
        *w = 13;
        *h = 13;
    } else if (value_is(type, "submit") || value_is(type, "reset") ||
               value_is(type, "button")) {
        const char *v = dom_get_attr(n, "value");
        int chars = v && *v ? (int)strlen(v) : 8;

        if (chars < 4) chars = 4;
        if (chars > 40) chars = 40;
        *w = chars * 8 + 16;
        *h = 24;
    } else if (value_is(type, "image")) {
        /* The image callback or width/height attributes decide this. */
    } else if (value_is(type, "range")) {
        *w = 160;
        *h = 20;
    } else {
        int chars = attr_positive(n, "size", 20, 200);

        *w = chars * 8 + 8;
        *h = 22;
    }
}

/* ---- the recursive builder --------------------------------------- */

struct build_env {
    struct lay_ctx *c;
    int in_link;
};

static struct lay_box *build_node(struct build_env *e, struct dom_node *n,
                                  const struct computed_style *parent_style,
                                  int depth);

static void fixup_children(struct lay_ctx *c, struct lay_box *b);
static void fixup_table(struct lay_ctx *c, struct lay_box *t);

static struct lay_box *build_text(struct lay_ctx *c, struct dom_node *n,
                                  const struct computed_style *cs)
{
    struct lay_box *b;
    unsigned long len;
    const char *p;

    p = fold_text(c->L, n->text, n->text_len, cs->text_transform, &len);
    if (!len)
        return 0;
    b = box_new(c, LAY_BOX_TEXT, cs, n);
    if (!b)
        return 0;
    b->text = p;
    b->text_len = (uint32_t)len;
    b->font = lay_font_for(cs);
    b->flags |= LAYF_ANON;
    if (!ws_collapses(cs->white_space))
        b->flags |= LAYF_PRE;
    c->L->stats.text_boxes++;
    return b;
}

static void mark_flags(struct lay_box *b, const struct computed_style *cs,
                       int in_link)
{
    if (cs->position != CSS_POSITION_STATIC) {
        b->flags |= LAYF_POSITIONED;
        if (cs->position == CSS_POSITION_ABSOLUTE ||
            cs->position == CSS_POSITION_FIXED) {
            b->flags |= LAYF_ABSOLUTE | LAYF_BFC;
            if (cs->position == CSS_POSITION_FIXED)
                b->flags |= LAYF_FIXED;
        } else if (cs->position == CSS_POSITION_STICKY) {
            b->flags |= LAYF_STICKY;
        }
        /* A positioned box with a used z-index other than auto starts a
         * stacking context. */
        if (!cs->z_auto)
            b->flags |= LAYF_STACK;
        b->z = cs->z_auto ? 0 : cs->z_index;
    }
    if (cs->css_float != CSS_FLOAT_NONE)
        b->flags |= LAYF_FLOAT | LAYF_BFC;
    if (cs->overflow != CSS_OVERFLOW_VISIBLE) {
        b->flags |= LAYF_CLIP | LAYF_BFC;
        if (cs->overflow == CSS_OVERFLOW_SCROLL ||
            cs->overflow == CSS_OVERFLOW_AUTO)
            b->flags |= LAYF_SCROLL;
    }
    if (cs->visibility != CSS_VISIBILITY_VISIBLE)
        b->flags |= LAYF_HIDDEN;
    if (!ws_collapses(cs->white_space))
        b->flags |= LAYF_PRE;
    if (cs->has_transform)
        b->flags |= LAYF_STACK;
    if (in_link)
        b->flags |= LAYF_LINK;
}

static struct lay_box *build_node(struct build_env *e, struct dom_node *n,
                                  const struct computed_style *parent_style,
                                  int depth)
{
    struct lay_ctx *c = e->c;
    const struct computed_style *cs;
    struct lay_box *b;
    struct dom_node *ch;
    uint32_t flags = 0;
    int disp, kind, was_link;
    char here;

    if ((uintptr_t)&here < c->stack_lo)
        c->stack_lo = (uintptr_t)&here;
    if ((uintptr_t)&here > c->stack_hi)
        c->stack_hi = (uintptr_t)&here;
    if (depth > c->depth_max)
        c->depth_max = depth;

    if (depth >= LAY_MAX_DEPTH) {
        c->L->trunc |= LAY_TRUNC_DEPTH;
        return 0;
    }
    if (n->type == DOM_TEXT)
        return build_text(c, n, parent_style);
    if (n->type != DOM_ELEMENT)
        return 0;

    cs = style_of(c->L, n);
    if (!cs)
        return 0;
    disp = used_display(cs);
    if (disp_is_none(disp))
        return 0;
    if (disp == CSS_DISPLAY_TABLE_COLUMN || disp == CSS_DISPLAY_TABLE_COLUMN_GROUP)
        return 0;   /* columns contribute width hints, not boxes */

    /* Replaced elements are atomic: they never get child boxes. */
    if (node_is_replaced(n)) {
        int32_t iw = 0, ih = 0;
        const char *input_type = n->tag_id == HTAG_INPUT
            ? dom_get_attr(n, "type") : 0;
        const char *src = n->tag_id == HTAG_VIDEO
            ? dom_get_attr(n, "poster")
            : ((n->tag_id == HTAG_IMG ||
                (n->tag_id == HTAG_INPUT &&
                 value_is(input_type, "image")))
               ? dom_get_attr(n, "src") : 0);

        b = box_new(c, LAY_BOX_REPLACED, cs, n);
        if (!b)
            return 0;
        b->flags |= LAYF_ATOMIC | LAYF_REPLACED;
        if (disp == CSS_DISPLAY_BLOCK || disp == CSS_DISPLAY_LIST_ITEM)
            b->flags &= ~LAYF_ATOMIC;
        mark_flags(b, cs, e->in_link);
        b->src = src;
        if (src && c->L->opt.image_size) {
            int w = 0, h = 0;

            if (c->L->opt.image_size(c->L->opt.image_ctx, src, &w, &h) &&
                w > 0 && h > 0) {
                iw = (int32_t)w;
                ih = (int32_t)h;
            }
        }
        if (!iw) attr_px(n, "width", &iw);
        if (!ih) attr_px(n, "height", &ih);
        if (n->tag_id == HTAG_CANVAS && n->canvas &&
            n->canvas->w > 0 && n->canvas->h > 0) {
            if (!iw) iw = n->canvas->w;
            if (!ih) ih = n->canvas->h;
        }
        control_intrinsic(n, &iw, &ih);
        b->intrinsic_w = iw;
        b->intrinsic_h = ih;
        return b;
    }

    kind = box_kind_for_display(disp, &flags);
    b = box_new(c, kind, cs, n);
    if (!b)
        return 0;
    b->flags |= flags;
    mark_flags(b, cs, e->in_link);
    if (n->tag_id == HTAG_BR)
        b->flags |= LAYF_BR;
    if (kind == LAY_BOX_TABLE)
        c->L->stats.tables++;

    was_link = e->in_link;
    if (n->tag_id == HTAG_A && dom_get_attr(n, "href"))
        e->in_link = 1;

    /* A list item gets a marker box; `outside` markers are recognised by
     * block layout and taken out of the flow, `inside` ones are folded
     * into the first line by the inline builder. */
    if (disp == CSS_DISPLAY_LIST_ITEM &&
        cs->list_style_type != CSS_LISTSTYLE_NONE) {
        struct lay_box *m = box_new(c, LAY_BOX_MARKER, cs, n);

        if (m) {
            m->flags |= LAYF_ANON;
            m->marker = marker_text(c->L, cs, n);
            m->font = lay_font_for(cs);
            box_append(b, m);
        }
    }

    for (ch = n->first_child; ch; ch = ch->next_sibling) {
        struct lay_box *cb = build_node(e, ch, cs, depth + 1);

        if (cb)
            box_append(b, cb);
        else if (c->L->trunc & (LAY_TRUNC_BOXES | LAY_TRUNC_MEMORY))
            break;
    }
    e->in_link = was_link;

    if (kind == LAY_BOX_TABLE)
        fixup_table(c, b);
    else if (kind != LAY_BOX_INLINE)
        fixup_children(c, b);
    return b;
}

/* ---- anonymous block boxes --------------------------------------- *
 *
 * CSS 2.1 9.2.1.1: when a block container has both block-level and
 * inline-level children, every run of inline-level children is wrapped
 * in an anonymous block box. Runs that are nothing but collapsible white
 * space are dropped instead - without that, every newline between two
 * <div>s would generate an empty line box and the page would gain a
 * blank line everywhere the author pretty-printed their HTML.
 */
static int box_is_blank_text(const struct lay_box *b)
{
    unsigned long i;

    if (b->kind != LAY_BOX_TEXT || (b->flags & LAYF_PRE))
        return 0;
    for (i = 0; i < b->text_len; i++)
        if (!ws_is_space((unsigned char)b->text[i]))
            return 0;
    return 1;
}

static void fixup_flex_grid_children(struct lay_ctx *c, struct lay_box *b)
{
    struct lay_box *ch, *nx, *anon = 0;
    struct lay_box *head = b->first_child;

    if (!head)
        return;
    b->first_child = b->last_child = 0;
    for (ch = head; ch; ch = nx) {
        nx = ch->next;
        ch->parent = 0;
        ch->next = ch->prev = 0;

        if (ch->kind == LAY_BOX_MARKER) {
            box_append(b, ch);
            continue;
        }
        if (ch->flags & LAYF_ABSOLUTE) {
            box_append(b, ch);
            continue;
        }
        ch->flags &= ~LAYF_FLOAT;
        if (ch->kind == LAY_BOX_TEXT) {
            if (box_is_blank_text(ch))
                continue;
            if (!anon) {
                anon = box_new(c, LAY_BOX_BLOCK, b->style, b->node);
                if (!anon) {
                    box_append(b, ch);
                    continue;
                }
                anon->flags |= LAYF_ANON | LAYF_IFC | LAYF_BFC;
                box_append(b, anon);
            }
            box_append(anon, ch);
            continue;
        }
        anon = 0;
        if (ch->kind == LAY_BOX_INLINE) {
            ch->kind = LAY_BOX_BLOCK;
            ch->flags &= ~LAYF_ATOMIC;
            ch->flags |= LAYF_BFC;
            fixup_children(c, ch);
        } else {
            ch->flags &= ~LAYF_ATOMIC;
            ch->flags |= LAYF_BFC;
        }
        box_append(b, ch);
    }
}

static void fixup_children(struct lay_ctx *c, struct lay_box *b)
{
    struct lay_box *ch, *nx, *anon = 0;
    struct lay_box *head = b->first_child, *tail = b->last_child;
    int has_block = 0;

    if (!head)
        return;
    if (b->style &&
        (b->style->display == CSS_DISPLAY_FLEX ||
         b->style->display == CSS_DISPLAY_INLINE_FLEX ||
         b->style->display == CSS_DISPLAY_GRID ||
         b->style->display == CSS_DISPLAY_INLINE_GRID)) {
        fixup_flex_grid_children(c, b);
        return;
    }
    for (ch = head; ch; ch = ch->next) {
        if (ch->kind == LAY_BOX_MARKER)
            continue;
        if (!box_is_inline_level(ch) && !box_is_out_of_flow(ch)) {
            has_block = 1;
            break;
        }
    }
    if (!has_block) {
        b->flags |= LAYF_IFC;
        return;
    }

    /* Rebuild the child list, wrapping inline runs. */
    b->first_child = b->last_child = 0;
    (void)tail;
    for (ch = head; ch; ch = nx) {
        nx = ch->next;
        ch->parent = 0;
        ch->next = ch->prev = 0;

        if (ch->kind == LAY_BOX_MARKER) {
            box_append(b, ch);
            continue;
        }
        if (box_is_inline_level(ch)) {
            if (box_is_blank_text(ch))
                continue;
            if (!anon) {
                anon = box_new(c, LAY_BOX_BLOCK, b->style, b->node);
                if (!anon) {
                    box_append(b, ch);
                    continue;
                }
                anon->flags |= LAYF_ANON | LAYF_IFC;
                box_append(b, anon);
            }
            box_append(anon, ch);
        } else if (box_is_out_of_flow(ch)) {
            /* Out-of-flow boxes are neutral: they join the inline run
             * they were written inside, so floats interact with the
             * right line boxes, and stand alone otherwise. */
            if (anon)
                box_append(anon, ch);
            else
                box_append(b, ch);
        } else {
            anon = 0;
            box_append(b, ch);
        }
    }
    /* An anonymous block that ended up holding nothing but blanks. */
    for (ch = b->first_child; ch; ch = nx) {
        nx = ch->next;
        if ((ch->flags & LAYF_ANON) && ch->kind == LAY_BOX_BLOCK &&
            !ch->first_child)
            box_unlink(ch);
    }
}

/* ---- anonymous table boxes --------------------------------------- */

static struct lay_box *anon_box(struct lay_ctx *c, int kind,
                                struct lay_box *parent)
{
    struct lay_box *b = box_new(c, kind, parent->style, parent->node);

    if (!b)
        return 0;
    b->flags |= LAYF_ANON | LAYF_TABLE_ANON;
    if (kind == LAY_BOX_TABLE_CELL)
        b->flags |= LAYF_BFC;
    box_append(parent, b);
    return b;
}

static void fixup_row(struct lay_ctx *c, struct lay_box *row)
{
    struct lay_box *ch, *nx, *cell = 0;
    struct lay_box *head = row->first_child;

    row->first_child = row->last_child = 0;
    for (ch = head; ch; ch = nx) {
        nx = ch->next;
        ch->parent = 0;
        ch->next = ch->prev = 0;
        if (ch->kind == LAY_BOX_TABLE_CELL) {
            cell = 0;
            box_append(row, ch);
            fixup_children(c, ch);
        } else {
            if (box_is_blank_text(ch))
                continue;
            if (!cell)
                cell = anon_box(c, LAY_BOX_TABLE_CELL, row);
            if (cell)
                box_append(cell, ch);
        }
    }
    for (ch = row->first_child; ch; ch = ch->next)
        if ((ch->flags & LAYF_TABLE_ANON) && ch->kind == LAY_BOX_TABLE_CELL)
            fixup_children(c, ch);
}

static void fixup_rowgroup(struct lay_ctx *c, struct lay_box *g)
{
    struct lay_box *ch, *nx, *row = 0;
    struct lay_box *head = g->first_child;

    g->first_child = g->last_child = 0;
    for (ch = head; ch; ch = nx) {
        nx = ch->next;
        ch->parent = 0;
        ch->next = ch->prev = 0;
        if (ch->kind == LAY_BOX_TABLE_ROW) {
            row = 0;
            box_append(g, ch);
            fixup_row(c, ch);
        } else {
            if (box_is_blank_text(ch))
                continue;
            if (!row)
                row = anon_box(c, LAY_BOX_TABLE_ROW, g);
            if (row)
                box_append(row, ch);
        }
    }
    for (ch = g->first_child; ch; ch = ch->next)
        if ((ch->flags & LAYF_TABLE_ANON) && ch->kind == LAY_BOX_TABLE_ROW)
            fixup_row(c, ch);
}

static void fixup_table(struct lay_ctx *c, struct lay_box *t)
{
    struct lay_box *ch, *nx, *grp = 0;
    struct lay_box *head = t->first_child;

    t->first_child = t->last_child = 0;
    for (ch = head; ch; ch = nx) {
        nx = ch->next;
        ch->parent = 0;
        ch->next = ch->prev = 0;

        switch (ch->kind) {
        case LAY_BOX_TABLE_ROWGROUP:
            grp = 0;
            box_append(t, ch);
            fixup_rowgroup(c, ch);
            break;
        case LAY_BOX_TABLE_ROW:
            if (!grp)
                grp = anon_box(c, LAY_BOX_TABLE_ROWGROUP, t);
            if (grp) {
                box_append(grp, ch);
                fixup_row(c, ch);
            }
            break;
        default:
            if (ch->style && ch->style->display == CSS_DISPLAY_TABLE_CAPTION) {
                /* Captions live beside the rows, not inside them. */
                box_append(t, ch);
                fixup_children(c, ch);
                break;
            }
            if (box_is_blank_text(ch))
                continue;
            {
                struct lay_box *row;

                if (!grp)
                    grp = anon_box(c, LAY_BOX_TABLE_ROWGROUP, t);
                if (!grp)
                    break;
                row = grp->last_child;
                if (!row || row->kind != LAY_BOX_TABLE_ROW ||
                    !(row->flags & LAYF_TABLE_ANON))
                    row = anon_box(c, LAY_BOX_TABLE_ROW, grp);
                if (!row)
                    break;
                {
                    struct lay_box *cell = row->last_child;

                    if (!cell || !(cell->flags & LAYF_TABLE_ANON))
                        cell = anon_box(c, LAY_BOX_TABLE_CELL, row);
                    if (cell)
                        box_append(cell, ch);
                }
            }
            break;
        }
    }
    /* Anonymous cells created above still need their own inline fixup. */
    for (ch = t->first_child; ch; ch = ch->next) {
        struct lay_box *r, *cell;

        if (ch->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = ch->first_child; r; r = r->next)
            for (cell = r->first_child; cell; cell = cell->next)
                if ((cell->flags & LAYF_TABLE_ANON) &&
                    cell->kind == LAY_BOX_TABLE_CELL && !(cell->flags & LAYF_IFC))
                    fixup_children(c, cell);
    }

    /* colspan / rowspan come from the HTML attributes; CSS has no such
     * properties, so computed_style cannot carry them. */
    for (ch = t->first_child; ch; ch = ch->next) {
        struct lay_box *r, *cell;

        if (ch->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = ch->first_child; r; r = r->next) {
            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t v;

                cell->colspan = 1;
                cell->rowspan = 1;
                if (cell->node && attr_px(cell->node, "colspan", &v) && v > 0)
                    cell->colspan = (uint16_t)(v > LAY_MAX_COLUMNS
                                               ? LAY_MAX_COLUMNS : v);
                if (cell->node && attr_px(cell->node, "rowspan", &v) && v > 0)
                    cell->rowspan = (uint16_t)(v > 1024 ? 1024 : v);
            }
        }
    }
}

/* ================================================================== *
 * 5  intrinsic widths
 *
 * The minimum content width is the widest thing that cannot be broken;
 * the maximum content width is what the content would occupy on one
 * infinitely long line. Shrink-to-fit boxes (floats, inline-blocks,
 * absolutes with auto width) and automatic table layout are both
 * defined in terms of these two numbers, so they are computed once here
 * and nowhere else.
 * ================================================================== */

static void translate_subtree(struct lay_box *b, int32_t dx, int32_t dy);
static void intrinsic_widths(struct lay_ctx *c, struct lay_box *b,
                             int32_t *minw, int32_t *maxw, int depth);
static void table_intrinsic(struct lay_ctx *c, struct lay_box *t,
                            int32_t *minw, int32_t *maxw, int depth);

/* Horizontal border+padding of a box. Percentages count as zero: their
 * base is the containing block width, which intrinsic sizing does not
 * have yet. */
static int32_t hedge_bp(const struct computed_style *cs)
{
    return used_len(cs->padding[CSS_LEFT], -1, 0) +
           used_len(cs->padding[CSS_RIGHT], -1, 0) +
           cs->border_width[CSS_LEFT] + cs->border_width[CSS_RIGHT];
}

static int32_t hedge_margin(const struct computed_style *cs)
{
    return used_len(cs->margin[CSS_LEFT], -1, 0) +
           used_len(cs->margin[CSS_RIGHT], -1, 0);
}

/* Text: widest unbreakable word, and the width of the whole run laid on
 * one line. */
static void text_intrinsic(const struct lay_box *b, int32_t *minw,
                           int32_t *sumw)
{
    unsigned long i = 0, start;
    int32_t widest = 0, total = 0, line = 0;
    int32_t step = text_char_step(b->font, b->style);
    int pre = (b->flags & LAYF_PRE) != 0;
    int wrap = ws_wraps(b->style->white_space);
    int char_min = (b->style->word_break == CSS_WORDBREAK_BREAK_ALL ||
                    b->style->overflow_wrap == CSS_OVERFLOWWRAP_ANYWHERE);
    int have_ch = 0;

    if (!pre) {
        while (i < b->text_len) {
            while (i < b->text_len && ws_is_space((unsigned char)b->text[i]))
                i++;
            start = i;
            while (i < b->text_len && !ws_is_space((unsigned char)b->text[i]))
                i++;
            if (i > start) {
                unsigned long wn = i - start;
                int32_t w = text_width_vis(b->font, b->style, wn);
                int32_t mw = char_min ? b->font->w : w;

                if (mw > widest)
                    widest = mw;
                if (have_ch)
                    total += step;         /* the collapsed space */
                total += (int32_t)((long)wn * (long)step);
                have_ch = 1;
            }
        }
        if (have_ch)
            total = total - step + b->font->w;
        if (!wrap)
            widest = total;   /* nowrap: nothing may break */
        *minw = widest;
        *sumw = total;
        return;
    }
    for (i = 0; i <= b->text_len; i++) {
        if (i == b->text_len || b->text[i] == '\n') {
            int32_t vis = have_ch ? (line - step + b->font->w) : 0;
            if (vis > widest) widest = vis;
            if (vis > total) total = vis;
            line = 0;
            have_ch = 0;
        } else {
            line += step;
            have_ch = 1;
        }
    }
    if (wrap) {
        int32_t run = 0;
        int run_ch = 0;

        widest = 0;
        for (i = 0; i < b->text_len; i++) {
            if (ws_is_space((unsigned char)b->text[i])) {
                int32_t vis = run_ch ? (run - step + b->font->w) : 0;
                if (vis > widest) widest = vis;
                run = 0;
                run_ch = 0;
            } else if (char_min) {
                if (b->font->w > widest)
                    widest = b->font->w;
            } else {
                run += step;
                run_ch = 1;
            }
        }
        if (run_ch) {
            int32_t vis = run - step + b->font->w;
            if (vis > widest) widest = vis;
        }
    }
    *minw = widest;
    *sumw = total;
}

/* Accumulate the inline content of a container into (min, running sum,
 * best sum). A forced break closes the running sum. */
static void inline_intrinsic(struct lay_ctx *c, struct lay_box *cont,
                             int32_t *minw, int32_t *run, int32_t *best,
                             int depth)
{
    struct lay_box *ch;

    if (depth >= LAY_MAX_DEPTH)
        return;
    for (ch = cont->first_child; ch; ch = ch->next) {
        int32_t m = 0, s = 0;

        if (ch->flags & LAYF_ABSOLUTE)
            continue;
        if (ch->flags & LAYF_BR) {
            if (*run > *best) *best = *run;
            *run = 0;
            continue;
        }
        if (ch->flags & LAYF_FLOAT) {
            intrinsic_widths(c, ch, &m, &s, depth + 1);
            if (m > *minw) *minw = m;
            *run += s;
            continue;
        }
        switch (ch->kind) {
        case LAY_BOX_TEXT:
            text_intrinsic(ch, &m, &s);
            if (m > *minw) *minw = m;
            *run += s;
            break;
        case LAY_BOX_MARKER: {
            int32_t w = ch->marker
                ? text_width(ch->font, ch->marker,
                             (unsigned long)strlen(ch->marker))
                : ch->font->w;

            w += ch->font->w / 2;
            if (w > *minw) *minw = w;
            *run += w;
            break;
        }
        case LAY_BOX_INLINE: {
            int32_t x = hedge_bp(ch->style) + hedge_margin(ch->style);

            *run += x;
            inline_intrinsic(c, ch, minw, run, best, depth + 1);
            break;
        }
        default:
            intrinsic_widths(c, ch, &m, &s, depth + 1);
            if (m > *minw) *minw = m;
            *run += s;
            break;
        }
    }
    if (*run > *best)
        *best = *run;
}

static int len_is_fixed_px(struct css_len l, int32_t *out)
{
    if (l.type == CSS_LEN_PX) {
        *out = l.v;
        return 1;
    }
    if (l.type == CSS_LEN_CALC && l.pct == 0) {
        *out = l.v;
        return 1;
    }
    return 0;
}

static void grid_intrinsic_item(struct lay_ctx *c, struct lay_box *ch,
                                const struct computed_style *cs, int ncols,
                                int32_t col_gap, int *io_col,
                                int32_t *cmin, int32_t *cmax, int depth)
{
    const struct computed_style *is = ch->style;
    int32_t m = 0, s = 0;
    int c0, span;

    if (is->grid_col_start > 0 && is->grid_col_end > is->grid_col_start)
        span = is->grid_col_end - is->grid_col_start;
    else
        span = is->grid_col_span >= 1 ? is->grid_col_span : 1;
    if (span > ncols) span = ncols;
    if (is->grid_col_start > 0) {
        c0 = is->grid_col_start - 1;
        if (c0 >= ncols) { c0 = ncols - 1; span = 1; }
        if (c0 + span > ncols) span = ncols - c0;
    } else if (is->grid_col_end > 0) {
        c0 = imax(0, is->grid_col_end - 1 - span);
    } else {
        if (*io_col + span > ncols)
            *io_col = 0;
        c0 = *io_col;
        *io_col += span;
        if (*io_col >= ncols)
            *io_col = 0;
    }
    intrinsic_widths(c, ch, &m, &s, depth + 1);
    if (span == 1) {
        if (cs->grid_ncols == 0 || cs->grid_cols[c0].kind != CSS_GRID_PX) {
            if (m > cmin[c0]) cmin[c0] = m;
            if (s > cmax[c0]) cmax[c0] = s;
        }
    } else {
        int32_t sm = 0, sx = 0;
        int k;
        for (k = 0; k < span; k++) {
            sm += cmin[c0 + k];
            sx += cmax[c0 + k];
        }
        sm += col_gap * (span - 1);
        sx += col_gap * (span - 1);
        if (m > sm) {
            int32_t per = (m - sm + span - 1) / span;
            for (k = 0; k < span; k++)
                if (cs->grid_ncols == 0 ||
                    cs->grid_cols[c0 + k].kind != CSS_GRID_PX)
                    cmin[c0 + k] += per;
        }
        if (s > sx) {
            int32_t per = (s - sx + span - 1) / span;
            for (k = 0; k < span; k++)
                if (cs->grid_ncols == 0 ||
                    cs->grid_cols[c0 + k].kind != CSS_GRID_PX)
                    cmax[c0 + k] += per;
        }
    }
}

static void grid_intrinsic(struct lay_ctx *c, struct lay_box *b,
                           int32_t *minw, int32_t *maxw, int depth)
{
    const struct computed_style *cs = b->style;
    int32_t cmin[CSS_GRID_MAX_COLS], cmax[CSS_GRID_MAX_COLS];
    int ncols = cs->grid_ncols > 0
        ? (cs->grid_ncols > CSS_GRID_MAX_COLS ? CSS_GRID_MAX_COLS : cs->grid_ncols)
        : 1;
    int32_t col_gap = cs->column_gap > 0 ? cs->column_gap
                                         : (cs->gap > 0 ? cs->gap : 0);
    struct lay_box *ch;
    struct lay_box **ord = 0;
    int col = 0, i, has_order = 0, cnt = 0;

    for (i = 0; i < ncols; i++) {
        if (cs->grid_ncols > 0 && cs->grid_cols[i].kind == CSS_GRID_PX) {
            int32_t v = imax(0, cs->grid_cols[i].val);
            cmin[i] = cmax[i] = v;
        } else {
            cmin[i] = cmax[i] = 0;
        }
    }
    for (ch = b->first_child; ch; ch = ch->next) {
        if (ch->kind == LAY_BOX_MARKER || (ch->flags & LAYF_ABSOLUTE))
            continue;
        cnt++;
        if (ch->style && ch->style->order != 0)
            has_order = 1;
    }
    if (has_order && cnt > 1) {
        ord = (struct lay_box **)calloc((unsigned long)cnt, sizeof(*ord));
        if (ord) {
            int idx = 0, k2;
            for (ch = b->first_child; ch; ch = ch->next)
                if (ch->kind != LAY_BOX_MARKER && !(ch->flags & LAYF_ABSOLUTE))
                    ord[idx++] = ch;
            for (idx = 1; idx < cnt; idx++) {
                struct lay_box *key = ord[idx];
                int32_t ko = key->style->order;
                k2 = idx - 1;
                while (k2 >= 0 && ord[k2]->style->order > ko) {
                    ord[k2 + 1] = ord[k2];
                    k2--;
                }
                ord[k2 + 1] = key;
            }
            for (idx = 0; idx < cnt; idx++)
                grid_intrinsic_item(c, ord[idx], cs, ncols, col_gap, &col,
                                    cmin, cmax, depth);
            free(ord);
            goto done_grid_cols;
        }
    }
    for (ch = b->first_child; ch; ch = ch->next) {
        if (ch->kind == LAY_BOX_MARKER || (ch->flags & LAYF_ABSOLUTE))
            continue;
        grid_intrinsic_item(c, ch, cs, ncols, col_gap, &col,
                            cmin, cmax, depth);
    }
done_grid_cols:
    *minw = *maxw = col_gap * (ncols - 1);
    for (i = 0; i < ncols; i++) {
        if (cmax[i] < cmin[i]) cmax[i] = cmin[i];
        *minw += cmin[i];
        *maxw += cmax[i];
    }
}

static void intrinsic_widths(struct lay_ctx *c, struct lay_box *b,
                             int32_t *minw, int32_t *maxw, int depth)
{
    const struct computed_style *cs = b->style;
    int32_t bp, extra, px = 0, inner_min = 0, inner_max = 0;
    struct lay_box *ch;
    char here;

    if ((uintptr_t)&here < c->stack_lo)
        c->stack_lo = (uintptr_t)&here;
    if ((uintptr_t)&here > c->stack_hi)
        c->stack_hi = (uintptr_t)&here;

    *minw = *maxw = 0;
    if (depth >= LAY_MAX_DEPTH) {
        c->L->trunc |= LAY_TRUNC_DEPTH;
        return;
    }
    if (b->kind == LAY_BOX_TEXT) {
        text_intrinsic(b, minw, maxw);
        return;
    }
    bp = hedge_bp(cs);
    extra = bp + hedge_margin(cs);

    if (b->kind == LAY_BOX_REPLACED) {
        int32_t w;

        if (len_is_fixed_px(cs->width, &px))
            w = content_dim(px, cs, bp);
        else if (cs->aspect_ratio > 0 && len_is_fixed_px(cs->height, &px)) {
            int32_t vbp = vedge_bp(cs);
            int32_t dh = clamp_height(content_dim(px, cs, vbp), cs, -1, vbp);
            w = ratio_width_from_height(cs, dh, bp, vbp);
        } else
            w = b->intrinsic_w;
        if (len_is_fixed_px(cs->max_width, &px)) {
            int32_t mx = content_dim(px, cs, bp);
            if (w > mx) w = mx;
        }
        if (len_is_fixed_px(cs->min_width, &px)) {
            int32_t mn = content_dim(px, cs, bp);
            if (w < mn) w = mn;
        }
        *minw = *maxw = imax(0, w) + extra;
        return;
    }
    if (b->kind == LAY_BOX_TABLE) {
        table_intrinsic(c, b, &inner_min, &inner_max, depth);
        *minw = inner_min + extra;
        *maxw = inner_max + extra;
        return;
    }
    if (len_is_fixed_px(cs->width, &px)) {
        inner_min = inner_max = content_dim(px, cs, bp);
    } else if (cs->display == CSS_DISPLAY_GRID ||
               cs->display == CSS_DISPLAY_INLINE_GRID) {
        grid_intrinsic(c, b, &inner_min, &inner_max, depth);
    } else if ((cs->display == CSS_DISPLAY_FLEX ||
                cs->display == CSS_DISPLAY_INLINE_FLEX) &&
               (cs->flex_direction == CSS_FLEXDIR_ROW ||
                cs->flex_direction == CSS_FLEXDIR_ROW_REVERSE)) {
        int32_t gap = cs->column_gap > 0 ? cs->column_gap
                                         : (cs->gap > 0 ? cs->gap : 0);
        int wrap = cs->flex_wrap == CSS_FLEXWRAP_WRAP ||
                   cs->flex_wrap == CSS_FLEXWRAP_WRAP_REVERSE;
        int n = 0;

        for (ch = b->first_child; ch; ch = ch->next) {
            int32_t m = 0, s = 0;

            if (ch->kind == LAY_BOX_MARKER || (ch->flags & LAYF_ABSOLUTE))
                continue;
            intrinsic_widths(c, ch, &m, &s, depth + 1);
            if (wrap) {
                if (m > inner_min) inner_min = m;
            } else {
                inner_min += m;
            }
            inner_max += s;
            n++;
        }
        if (n > 1) {
            if (!wrap) inner_min += gap * (n - 1);
            inner_max += gap * (n - 1);
        }
    } else if (b->flags & LAYF_IFC) {
        int32_t run = 0, best = 0;

        inline_intrinsic(c, b, &inner_min, &run, &best, depth + 1);
        inner_max = best;
    } else {
        for (ch = b->first_child; ch; ch = ch->next) {
            int32_t m = 0, s = 0;

            if (ch->kind == LAY_BOX_MARKER || (ch->flags & LAYF_ABSOLUTE))
                continue;
            intrinsic_widths(c, ch, &m, &s, depth + 1);
            if (m > inner_min) inner_min = m;
            if (s > inner_max) inner_max = s;
        }
    }
    if (!len_is_fixed_px(cs->width, &px) &&
        cs->aspect_ratio > 0 && len_is_fixed_px(cs->height, &px)) {
        int32_t vbp = vedge_bp(cs);
        int32_t dh = clamp_height(content_dim(px, cs, vbp), cs, -1, vbp);
        int32_t rw = ratio_width_from_height(cs, dh, bp, vbp);
        if (rw > inner_min) inner_min = rw;
        if (rw > inner_max) inner_max = rw;
    }
    if (len_is_fixed_px(cs->min_width, &px)) {
        int32_t mn = content_dim(px, cs, bp);
        if (inner_min < mn) inner_min = mn;
        if (inner_max < mn) inner_max = mn;
    }
    if (len_is_fixed_px(cs->max_width, &px)) {
        int32_t mx = content_dim(px, cs, bp);
        if (inner_min > mx) inner_min = mx;
        if (inner_max > mx) inner_max = mx;
    }
    if (inner_max < inner_min)
        inner_max = inner_min;
    *minw = inner_min + extra;
    *maxw = inner_max + extra;
}

/* ================================================================== *
 * 6  floats
 * ================================================================== */

static void line_edges(struct lay_ctx *c, struct lay_bfc *bfc,
                       int32_t y, int32_t h, int32_t lo, int32_t hi,
                       int32_t *l, int32_t *r)
{
    int i;

    *l = lo;
    *r = hi;
    if (h <= 0)
        h = 1;
    for (i = bfc->base; i < c->nfl; i++) {
        struct lay_float *f = &c->floats[i];

        if (f->y >= y + h || f->y + f->h <= y)
            continue;
        if (f->side == CSS_FLOAT_LEFT) {
            if (f->x + f->w > *l)
                *l = f->x + f->w;
        } else {
            if (f->x < *r)
                *r = f->x;
        }
    }
    if (*r < *l)
        *r = *l;
}

/* The next y at which the set of overlapping floats changes. */
static int32_t float_next_y(struct lay_ctx *c, struct lay_bfc *bfc, int32_t y)
{
    int i;
    int32_t best = 0;
    int have = 0;

    for (i = bfc->base; i < c->nfl; i++) {
        int32_t bot = c->floats[i].y + c->floats[i].h;

        if (bot > y && (!have || bot < best)) {
            best = bot;
            have = 1;
        }
    }
    return have ? best : y;
}

static int32_t clear_past(struct lay_ctx *c, struct lay_bfc *bfc, int clear,
                          int32_t y)
{
    int i;

    if (clear == CSS_CLEAR_NONE)
        return y;
    for (i = bfc->base; i < c->nfl; i++) {
        struct lay_float *f = &c->floats[i];
        int32_t bot = f->y + f->h;

        if (clear == CSS_CLEAR_LEFT && f->side != CSS_FLOAT_LEFT) continue;
        if (clear == CSS_CLEAR_RIGHT && f->side != CSS_FLOAT_RIGHT) continue;
        if (bot > y)
            y = bot;
    }
    return y;
}

static int32_t float_lowest(struct lay_ctx *c, struct lay_bfc *bfc)
{
    int i;
    int32_t y = 0;
    int have = 0;

    for (i = bfc->base; i < c->nfl; i++) {
        int32_t bot = c->floats[i].y + c->floats[i].h;

        if (!have || bot > y) { y = bot; have = 1; }
    }
    return have ? y : 0;
}

/* Place an already-laid-out float: find the highest y at or below the
 * hint where its margin box fits between the floats already placed, then
 * translate the whole subtree there. */
static void float_place(struct lay_ctx *c, struct lay_bfc *bfc,
                        struct lay_box *b, int32_t y)
{
    lay_rect mb = lay_margin_rect(b);
    int32_t l, r, tx;
    int side = b->style->css_float;
    int guard = 0;

    y = clear_past(c, bfc, b->style->clear, y);
    for (;;) {
        line_edges(c, bfc, y, mb.h > 0 ? mb.h : 1, bfc->cx, bfc->cx + bfc->cw,
                   &l, &r);
        if (r - l >= mb.w || guard++ > 4096)
            break;
        {
            int32_t ny = float_next_y(c, bfc, y);

            if (ny <= y)
                break;
            y = ny;
        }
    }
    tx = (side == CSS_FLOAT_LEFT) ? l : r - mb.w;
    translate_subtree(b, tx - mb.x, y - mb.y);

    if (c->nfl < LAY_MAX_FLOATS) {
        struct lay_float *f = &c->floats[c->nfl++];

        mb = lay_margin_rect(b);
        f->x = mb.x;
        f->y = mb.y;
        f->w = mb.w;
        f->h = mb.h;
        f->side = (uint8_t)side;
        c->L->stats.floats++;
    } else {
        c->L->trunc |= LAY_TRUNC_FLOATS;
    }
}

/* Iterative pre-order walk, so a 10 000 deep subtree costs time and no
 * stack at all. */
static void translate_subtree(struct lay_box *b, int32_t dx, int32_t dy)
{
    struct lay_box *n = b;

    if (!dx && !dy)
        return;
    for (;;) {
        n->x += dx;
        n->y += dy;
        n->clip_x += dx;
        n->clip_y += dy;
        n->ink_x += dx;
        n->ink_y += dy;
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != b && !n->next)
            n = n->parent;
        if (n == b)
            break;
        n = n->next;
    }
}

/* A relative offset moves the box and its descendants, except for a fixed
 * descendant: that box is anchored to the viewport, and its whole subtree
 * must remain together at that viewport-relative position. */
static void translate_relative_subtree(struct lay_box *b,
                                       int32_t dx, int32_t dy)
{
    struct lay_box *n = b;

    if (!dx && !dy)
        return;
    for (;;) {
        int fixed_boundary = n != b && (n->flags & LAYF_FIXED);

        if (!fixed_boundary) {
            n->x += dx;
            n->y += dy;
            n->clip_x += dx;
            n->clip_y += dy;
            n->ink_x += dx;
            n->ink_y += dy;
        }
        if (!fixed_boundary && n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != b && !n->next)
            n = n->parent;
        if (n == b)
            break;
        n = n->next;
    }
}

/* ================================================================== *
 * 7  inline formatting: line boxes
 *
 * The block hands its inline children to an ifc, which consumes them in
 * document order and replaces them with a list of LAY_BOX_LINE children.
 * Text nodes are split into as many LAY_BOX_TEXT runs as there are
 * lines; an inline element that spans four lines produces four
 * LAY_BOX_INLINE fragments, the first drawing its left border and the
 * last its right one.
 * ================================================================== */

struct lay_ifc {
    struct lay_ctx *c;
    struct lay_box *block;
    struct lay_bfc *bfc;
    int32_t cx, cw;              /* the block content box               */
    int32_t y;                   /* top of the line being built         */
    int frag_base, rec_base;
    int16_t open[LAY_MAX_INLINE_NEST];
    int nopen;
    int32_t pen;                 /* x offset from the line content left */
    int32_t line_l, line_r;      /* absolute edges of the current line  */
    int32_t strut_asc, strut_desc;
    int32_t strut_font_asc, strut_font_desc;
    int pending_space;
    const struct computed_style *sp_style;
    const struct font *sp_font;
    const char *sp_text;      /* the space in the source, for coalescing */
    int16_t sp_rec;
    int nlines;
    int32_t max_w;
    int first_line;
    int last_forced;
    int depth;
};

static int32_t layout_block_box(struct lay_ctx *c, struct lay_box *b,
                                int32_t cb_x, int32_t cb_w, int32_t cb_h,
                                int32_t y, struct lay_bfc *bfc,
                                struct lay_marg *io_open, int skip_top,
                                int mode, int depth);
static void layout_standalone(struct lay_ctx *c, struct lay_box *b,
                              int32_t x, int32_t y, int32_t avail_w,
                              int shrink, int depth);
static void record_out_of_flow(struct lay_ctx *c, struct lay_box *b,
                               int32_t sx, int32_t sy);
static void resolve_replaced(struct lay_ctx *c, struct lay_box *b,
                             int32_t cb_w, int32_t cb_h);

static int ifc_has_content(const struct lay_ifc *f)
{
    return f->c->nfrag > f->frag_base || f->c->nrec > f->rec_base;
}

static void ifc_start_line(struct lay_ifc *f)
{
    int32_t h = f->strut_asc + f->strut_desc;

    line_edges(f->c, f->bfc, f->y, h, f->cx, f->cx + f->cw,
               &f->line_l, &f->line_r);
    f->pen = 0;
    if (f->first_line) {
        int32_t ind = used_len(f->block->style->text_indent, f->cw, 0);

        if (ind > 0)
            f->pen = ind;
    }
}

static int32_t ifc_avail(const struct lay_ifc *f)
{
    int32_t a = f->line_r - f->line_l;

    return a > 0 ? a : 0;
}

/* ---- fragments --------------------------------------------------- */

static struct lay_frag *ifc_frag(struct lay_ifc *f)
{
    struct lay_ctx *c = f->c;
    struct lay_frag *fr;

    if (c->nfrag >= LAY_MAX_FRAGS) {
        c->L->trunc |= LAY_TRUNC_LINES;
        return 0;
    }
    fr = &c->frags[c->nfrag++];
    memset(fr, 0, sizeof *fr);
    fr->rec = (int16_t)(f->nopen ? f->open[f->nopen - 1] : -1);
    return fr;
}

static void frag_metrics(struct lay_ifc *f, struct lay_frag *fr,
                         const struct computed_style *cs,
                         const struct font *font, int32_t h)
{
    int32_t asc, desc, fs = cs->font_size;

    if (font) {
        half_leading(cs, font, &asc, &desc);
    } else {
        asc = h;
        desc = 0;
    }
    fr->valign = cs->vertical_align;
    switch (cs->vertical_align) {
    case CSS_VALIGN_SUB:
        fr->shift = -(fs / 5);
        break;
    case CSS_VALIGN_SUPER:
        fr->shift = fs / 3;
        break;
    case CSS_VALIGN_LENGTH:
        fr->shift = cs->vertical_align_px;
        break;
    default:
        fr->shift = 0;
        break;
    }
    fr->asc = asc;
    fr->desc = desc;
    fr->asc0 = asc;
    (void)f;
}

static void ifc_commit(struct lay_ifc *f, int forced);

static void ifc_break(struct lay_ifc *f, int forced)
{
    ifc_commit(f, forced);
    f->pending_space = 0;
    ifc_start_line(f);
}

static void ifc_set_space(struct lay_ifc *f, const struct lay_box *tb,
                          const char *at)
{
    f->pending_space = 1;
    f->sp_style = tb->style;
    f->sp_font = tb->font;
    f->sp_text = at;
    f->sp_rec = (int16_t)(f->nopen ? f->open[f->nopen - 1] : -1);
}

/* Emit one already-measured run of characters. */
static void ifc_emit(struct lay_ifc *f, const struct lay_box *tb,
                     const char *s, unsigned long n, int32_t w, int is_space)
{
    struct lay_frag *fr = ifc_frag(f);

    if (!fr)
        return;
    fr->kind = LAY_FRAG_TEXT;
    fr->text = s;
    fr->len = (uint32_t)n;
    fr->font = tb->font;
    fr->style = tb->style;
    fr->node = tb->node;
    fr->x = f->pen;
    fr->w = w;
    fr->is_space = (uint8_t)is_space;
    frag_metrics(f, fr, tb->style, tb->font, 0);
    f->pen += w;
}

/* The advance of the collapsed word separator awaiting emission: one
 * character step plus the word-spacing of the element it belongs to.
 * Clamped at zero - a negative-width separator would move the pen
 * backwards and corrupt fragment metrics. */
static int32_t ifc_space_advance(const struct lay_ifc *f)
{
    int32_t ws = f->sp_style ? f->sp_style->word_spacing : 0;
    int32_t adv = text_char_step(f->sp_font, f->sp_style) + ws;
    return adv > 0 ? adv : 0;
}

static void ifc_emit_space(struct lay_ifc *f)
{
    struct lay_frag *fr;

    if (!f->pending_space || !f->sp_font)
        return;
    fr = ifc_frag(f);
    f->pending_space = 0;
    if (!fr)
        return;
    fr->kind = LAY_FRAG_TEXT;
    /* A slice of the source rather than a literal, so that a word, the
     * space after it and the next word coalesce into one glyph run. */
    fr->text = f->sp_text ? f->sp_text : " ";
    fr->len = 1;
    fr->font = f->sp_font;
    fr->style = f->sp_style;
    fr->rec = f->sp_rec;
    fr->x = f->pen;
    fr->w = ifc_space_advance(f);
    fr->is_space = 1;
    frag_metrics(f, fr, f->sp_style, f->sp_font, 0);
    f->pen += fr->w;
}

/* Add a word, breaking the line before it when it does not fit and
 * breaking the word itself when it cannot fit on a line of its own. */
static void ifc_add_word(struct lay_ifc *f, struct lay_box *tb,
                         const char *s, unsigned long n, int wrap)
{
    const struct font *font = tb->font;
    int32_t step = text_char_step(font, tb->style);
    int32_t w_adv = text_width_adv(font, tb->style, n);
    int32_t w_vis = text_width_vis(font, tb->style, n);
    int32_t sp = f->pending_space ? ifc_space_advance(f) : 0;
    int break_all = tb->style->word_break == CSS_WORDBREAK_BREAK_ALL;
    int guard = 0;

    for (;;) {
        int32_t avail = ifc_avail(f);

        if (!wrap || f->pen + sp + w_vis <= avail) {
            ifc_emit_space(f);
            ifc_emit(f, tb, s, n, w_adv, 0);
            return;
        }
        if (ifc_has_content(f) && f->pen > 0 &&
            (!break_all || f->pen + sp + font->w > avail)) {
            f->pending_space = 0;
            ifc_break(f, 0);
            sp = 0;
            if (guard++ > 4)
                wrap = 0;      /* a zero-width line: stop retrying */
            continue;
        }
        /* Break within the word at the last character that fits on this
         * line, never fewer than one. */
        {
            int32_t space_left = avail - (f->pen + sp);
            unsigned long fit = 1;

            if (space_left >= font->w && step > 0)
                fit = 1 + (unsigned long)((space_left - font->w) / step);
            if (fit < 1) fit = 1;
            if (fit >= n) {
                ifc_emit_space(f);
                ifc_emit(f, tb, s, n, w_adv, 0);
                return;
            }
            ifc_emit_space(f);
            ifc_emit(f, tb, s, fit, text_width_adv(font, tb->style, fit), 0);
            s += fit;
            n -= fit;
            w_adv = text_width_adv(font, tb->style, n);
            w_vis = text_width_vis(font, tb->style, n);
            ifc_break(f, 0);
            sp = 0;
            if (guard++ > 100000) {
                f->c->L->trunc |= LAY_TRUNC_LINES;
                return;
            }
        }
    }
}

static void ifc_add_text(struct lay_ifc *f, struct lay_box *tb)
{
    const char *t = tb->text;
    unsigned long n = tb->text_len, i = 0, j;
    int ws = tb->style->white_space;
    int collapse = ws_collapses(ws);
    int wrap = ws_wraps(ws);
    int keepnl = ws_keeps_newline(ws);

    while (i < n) {
        if (ws_is_space((unsigned char)t[i])) {
            if (collapse) {
                const char *sp_at = t + i;
                int nl = 0;

                j = i;
                while (j < n && ws_is_space((unsigned char)t[j])) {
                    if (t[j] == '\n')
                        nl = 1;
                    j++;
                }
                i = j;
                if (nl && keepnl) {          /* pre-line */
                    f->pending_space = 0;
                    ifc_break(f, 1);
                    f->last_forced = 1;
                    continue;
                }
                if (ifc_has_content(f) || f->pen > 0)
                    ifc_set_space(f, tb, sp_at);
                continue;
            }
            /* preserved white space */
            if (t[i] == '\n') {
                ifc_break(f, 1);
                f->last_forced = 1;
                i++;
                continue;
            }
            if (t[i] == '\r') {
                i++;
                continue;
            }
            j = i;
            while (j < n && ws_is_space((unsigned char)t[j]) &&
                   t[j] != '\n' && t[j] != '\r')
                j++;
            {
                unsigned long len = j - i;
                int32_t w = text_width_adv(tb->font, tb->style, len) +
                            tb->style->word_spacing * (int32_t)len;

                /* Preserved spaces are NOT trimmed at the end of a
                 * line - that is the whole point of `pre` - so they are
                 * marked 2 rather than 1. Each one is a word separator,
                 * so word-spacing applies to all of them. */
                if (w < 0)
                    w = 0;
                ifc_emit(f, tb, t + i, len, w, 2);
            }
            i = j;
            continue;
        }
        j = i;
        while (j < n && !ws_is_space((unsigned char)t[j]))
            j++;
        ifc_add_word(f, tb, t + i, j - i, wrap);
        f->last_forced = 0;
        i = j;
    }
}

/* The baseline of an atomic inline is the baseline of its last in-flow
 * line box (CSS 2.1 10.8.1); a replaced element, or a box that clips its
 * overflow, uses its bottom margin edge instead. Returns 0 when there is
 * no line box to use. */
static int atomic_baseline(struct lay_box *b, int32_t *out)
{
    struct lay_box *n = b;
    int32_t best = 0;
    int have = 0, guard = 0;

    if (b->kind == LAY_BOX_REPLACED || (b->flags & LAYF_CLIP))
        return 0;
    for (;;) {
        if (guard++ > 200000)
            break;
        if (n->kind == LAY_BOX_LINE) {
            best = n->y + n->baseline;
            have = 1;
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != b && !n->next) n = n->parent;
        if (n == b) break;
        n = n->next;
    }
    if (have)
        *out = best;
    return have;
}

static void ifc_add_atomic(struct lay_ifc *f, struct lay_box *b)
{
    lay_rect mb;
    int32_t w, sp;
    struct lay_frag *fr;
    int wrap = ws_wraps(f->block->style->white_space);

    /* Lay the box out first: an inline-level atomic box is sized by its
     * own content, shrink to fit, before it can be positioned. */
    if (b->kind == LAY_BOX_REPLACED)
        resolve_replaced(f->c, b, f->cw, -1);
    else
        layout_standalone(f->c, b, f->line_l, f->y, ifc_avail(f), 1,
                          f->depth + 1);
    mb = lay_margin_rect(b);
    w = mb.w;
    sp = f->pending_space ? ifc_space_advance(f) : 0;
    if (wrap && f->pen + sp + w > ifc_avail(f) && f->pen > 0 &&
        ifc_has_content(f)) {
        f->pending_space = 0;
        ifc_break(f, 0);
        sp = 0;
    }
    ifc_emit_space(f);
    fr = ifc_frag(f);
    if (!fr)
        return;
    fr->kind = LAY_FRAG_ATOMIC;
    fr->box = b;
    fr->style = b->style;
    fr->node = b->node;
    fr->x = f->pen;
    fr->w = w;
    fr->valign = b->style->vertical_align;
    /* An atomic inline aligns its MARGIN box bottom with the baseline,
     * unless it has in-flow line boxes of its own, in which case the
     * baseline of its last line is used. */
    {
        int32_t bl;

        if (atomic_baseline(b, &bl) && bl - mb.y >= 0 && bl - mb.y <= mb.h)
            fr->asc = bl - mb.y;
        else
            fr->asc = mb.h;
    }
    fr->desc = mb.h - fr->asc;
    fr->asc0 = fr->asc;
    switch (fr->valign) {
    case CSS_VALIGN_SUB:    fr->shift = -(b->style->font_size / 5); break;
    case CSS_VALIGN_SUPER:  fr->shift = b->style->font_size / 3; break;
    case CSS_VALIGN_LENGTH: fr->shift = b->style->vertical_align_px; break;
    default:                fr->shift = 0; break;
    }
    f->pen += w;
    f->last_forced = 0;
}

static void ifc_add_marker(struct lay_ifc *f, struct lay_box *b)
{
    struct lay_frag *fr = ifc_frag(f);
    int32_t w;

    if (!fr)
        return;
    w = b->marker ? text_width(b->font, b->marker,
                               (unsigned long)strlen(b->marker))
                  : b->font->w;
    w += b->font->w / 2;
    fr->kind = LAY_FRAG_MARKER;
    fr->box = b;
    fr->text = b->marker;
    fr->len = b->marker ? (uint32_t)strlen(b->marker) : 0;
    fr->font = b->font;
    fr->style = b->style;
    fr->node = b->node;
    fr->x = f->pen;
    fr->w = w;
    frag_metrics(f, fr, b->style, b->font, 0);
    f->pen += w;
}

/* ---- inline boxes ------------------------------------------------ */

static int ifc_open_rec(struct lay_ifc *f, struct lay_box *src)
{
    struct lay_ctx *c = f->c;
    struct lay_rec *r;
    const struct computed_style *cs = src->style;

    if (c->nrec >= LAY_MAX_FRAGS / 4 || f->nopen >= LAY_MAX_INLINE_NEST) {
        c->L->trunc |= LAY_TRUNC_NEST;
        return -1;
    }
    r = &c->recs[c->nrec];
    memset(r, 0, sizeof *r);
    r->parent = (int16_t)(f->nopen ? f->open[f->nopen - 1] : -1);
    r->style = cs;
    r->node = src->node;
    r->open = 1;
    r->flags = (src->flags & (LAYF_LINK | LAYF_HIDDEN)) | LAYF_FIRST_FRAG;
    f->pen += used_len(cs->margin[CSS_LEFT], f->cw, 0) +
              cs->border_width[CSS_LEFT] +
              used_len(cs->padding[CSS_LEFT], f->cw, 0);
    r->x0 = f->pen - (used_len(cs->margin[CSS_LEFT], f->cw, 0) +
                      cs->border_width[CSS_LEFT] +
                      used_len(cs->padding[CSS_LEFT], f->cw, 0));
    f->open[f->nopen++] = (int16_t)c->nrec;
    c->nrec++;
    return c->nrec - 1;
}

static void ifc_close_rec(struct lay_ifc *f)
{
    struct lay_ctx *c = f->c;
    struct lay_rec *r;
    const struct computed_style *cs;

    if (!f->nopen)
        return;
    r = &c->recs[f->open[--f->nopen]];
    cs = r->style;
    f->pen += used_len(cs->padding[CSS_RIGHT], f->cw, 0) +
              cs->border_width[CSS_RIGHT] +
              used_len(cs->margin[CSS_RIGHT], f->cw, 0);
    r->x1 = f->pen;
    r->open = 0;
    r->flags |= LAYF_LAST_FRAG;
}

/* ---- committing a line ------------------------------------------- */

/* Create the box for inline record `idx`, and any of its ancestors that
 * do not have one yet, parent first. Doing this lazily - at the moment
 * the first fragment that needs it appears - is what keeps the line
 * box's children in document order, which hit testing and the painter
 * both rely on. */
static struct lay_box *ensure_rec(struct lay_ifc *f, int idx,
                                  struct lay_box *line, int32_t base_y)
{
    struct lay_ctx *c = f->c;
    int16_t chain[LAY_MAX_INLINE_NEST];
    int n = 0, k, orig = idx;

    if (idx < 0)
        return line;
    while (idx >= 0 && n < LAY_MAX_INLINE_NEST) {
        if (c->recs[idx].box)
            break;
        chain[n++] = (int16_t)idx;
        idx = c->recs[idx].parent;
    }
    for (k = n - 1; k >= 0; k--) {
        struct lay_rec *r = &c->recs[chain[k]];
        const struct font *rf = lay_font_for(r->style);
        struct lay_box *ib, *par;
        int32_t ml, bl, pl, mr, br, pr, x0;

        ml = (r->flags & LAYF_FIRST_FRAG)
            ? used_len(r->style->margin[CSS_LEFT], f->cw, 0) : 0;
        bl = (r->flags & LAYF_FIRST_FRAG) ? r->style->border_width[CSS_LEFT] : 0;
        pl = (r->flags & LAYF_FIRST_FRAG)
            ? used_len(r->style->padding[CSS_LEFT], f->cw, 0) : 0;
        mr = (r->flags & LAYF_LAST_FRAG)
            ? used_len(r->style->margin[CSS_RIGHT], f->cw, 0) : 0;
        br = (r->flags & LAYF_LAST_FRAG) ? r->style->border_width[CSS_RIGHT] : 0;
        pr = (r->flags & LAYF_LAST_FRAG)
            ? used_len(r->style->padding[CSS_RIGHT], f->cw, 0) : 0;

        ib = box_new(c, LAY_BOX_INLINE, r->style, r->node);
        if (!ib)
            return line;
        ib->flags |= r->flags;
        x0 = line->x + r->x0;
        ib->x = x0 + ml + bl + pl;
        ib->w = imax(0, (r->x1 - r->x0) - (ml + bl + pl + mr + br + pr));
        ib->y = base_y - font_ascent(rf);
        ib->h = font_line_height(rf);
        ib->baseline = font_ascent(rf);
        ib->pad[CSS_LEFT] = pl;
        ib->pad[CSS_RIGHT] = pr;
        ib->pad[CSS_TOP] = used_len(r->style->padding[CSS_TOP], f->cw, 0);
        ib->pad[CSS_BOTTOM] = used_len(r->style->padding[CSS_BOTTOM], f->cw, 0);
        ib->bord[CSS_LEFT] = bl;
        ib->bord[CSS_RIGHT] = br;
        ib->bord[CSS_TOP] = r->style->border_width[CSS_TOP];
        ib->bord[CSS_BOTTOM] = r->style->border_width[CSS_BOTTOM];
        ib->font = rf;
        r->box = ib;
        par = r->parent >= 0 && c->recs[r->parent].box
            ? c->recs[r->parent].box : line;
        box_append(par, ib);
    }
    return c->recs[orig].box ? c->recs[orig].box : line;
}

static void ifc_commit(struct lay_ifc *f, int forced)
{
    struct lay_ctx *c = f->c;
    struct lay_box *line;
    struct lay_frag *fg = c->frags + f->frag_base;
    int nf = c->nfrag - f->frag_base;
    int nr = c->nrec - f->rec_base;
    int i, k, nsp = 0;
    int32_t max_asc, max_desc, line_w, avail, off = 0, extra = 0, base_y;
    int justify;

    /* Trailing COLLAPSIBLE spaces do not occupy the end of a line;
     * preserved ones (is_space == 2) do. */
    while (nf > 0 && fg[nf - 1].is_space == 1)
        nf--;

    /* Coalesce runs that are contiguous in the source and share a face,
     * a style and an inline parent. Words are measured one at a time
     * because that is how line breaking works, but a line of ordinary
     * prose then becomes ONE text box instead of one per word - which is
     * the difference between 180 000 boxes on a long article and 30 000.
     * Justified text keeps its spaces separate; they are what stretches. */
    if (f->block->style->text_align != CSS_TEXTALIGN_JUSTIFY) {
        int w = 0;

        for (i = 0; i < nf; i++) {
            if (w > 0) {
                struct lay_frag *pv = &fg[w - 1];
                struct lay_frag *cu = &fg[i];

                if (pv->kind == LAY_FRAG_TEXT && cu->kind == LAY_FRAG_TEXT &&
                    pv->font == cu->font && pv->style == cu->style &&
                    pv->rec == cu->rec && pv->valign == cu->valign &&
                    pv->text + pv->len == cu->text &&
                    pv->x + pv->w == cu->x) {
                    pv->len += cu->len;
                    pv->w += cu->w;
                    if (!cu->is_space || !pv->is_space)
                        pv->is_space = 0;
                    continue;
                }
            }
            if (w != i)
                fg[w] = fg[i];
            w++;
        }
        nf = w;
    }
    if (!nf && !nr) {
        if (!forced)
            return;
    }
    if (f->nlines >= LAY_MAX_LINES) {
        c->L->trunc |= LAY_TRUNC_LINES;
        c->nfrag = f->frag_base;
        return;
    }

    avail = ifc_avail(f);
    if ((f->block->flags & LAYF_CLIP) &&
        f->block->style->text_overflow == CSS_TEXTOVERFLOW_ELLIPSIS &&
        !ws_wraps(f->block->style->white_space) && nf > 0 && avail > 0) {
        struct lay_frag *lf = &fg[nf - 1];
        int32_t vis_end = lf->x + lf->w;

        if (lf->kind == LAY_FRAG_TEXT && lf->font && lf->len > 0 &&
            lf->style && lf->style->letter_spacing != 0)
            vis_end = lf->x + text_width_vis(lf->font, lf->style, lf->len);
        if (vis_end > avail) {
            const struct font *ef = lay_font_for(f->block->style);
            const struct computed_style *es = f->block->style;
            struct dom_node *ell_node = f->block->node;
            int16_t ell_rec = -1;
            int32_t ell_w = text_width_vis(ef, es, 3);
            int new_nf = 0;

            for (i = 0; i < nf; i++) {
                const struct font *cf = fg[i].font ? fg[i].font : ef;
                const struct computed_style *cs_i =
                    fg[i].style ? fg[i].style : es;
                int32_t step = text_char_step(cf, cs_i);
                int32_t ew = text_width_vis(cf, cs_i, 3);

                if (fg[i].kind != LAY_FRAG_TEXT) {
                    if (fg[i].x + fg[i].w + ew <= avail) {
                        new_nf = i + 1;
                        continue;
                    }
                    ef = cf; es = cs_i; ell_w = ew;
                    ell_node = fg[i].node; ell_rec = fg[i].rec;
                    new_nf = i;
                    break;
                }
                if (fg[i].x + (int32_t)fg[i].len * step + ew <= avail) {
                    new_nf = i + 1;
                    continue;
                }
                {
                    int32_t rem = avail - ew - fg[i].x;
                    unsigned long keep = (rem > 0 && step > 0)
                        ? (unsigned long)(rem / step) : 0;

                    if (keep > fg[i].len)
                        keep = fg[i].len;
                    ef = cf; es = cs_i; ell_w = ew;
                    ell_node = fg[i].node; ell_rec = fg[i].rec;
                    if (keep > 0) {
                        fg[i].len = (uint32_t)keep;
                        fg[i].w = (int32_t)((long)keep * (long)step);
                        new_nf = i + 1;
                    } else {
                        new_nf = i;
                        if (new_nf == 0)
                            ell_rec = -1;
                    }
                }
                break;
            }
            if (new_nf == nf) {
                if (ifc_frag(f)) {
                    fg = c->frags + f->frag_base;
                    nf = new_nf + 1;
                }
            } else {
                nf = new_nf + 1;
            }
            c->nfrag = f->frag_base + nf;
            {
                struct lay_frag *efr = &fg[nf - 1];
                int32_t ex = (nf > 1) ? fg[nf - 2].x + fg[nf - 2].w : 0;

                memset(efr, 0, sizeof(*efr));
                efr->kind = LAY_FRAG_TEXT;
                efr->text = "...";
                efr->len = 3;
                efr->font = ef;
                efr->style = es;
                efr->node = ell_node;
                efr->rec = ell_rec;
                efr->x = ex;
                efr->w = ell_w;
                frag_metrics(f, efr, es, ef, 0);
                f->pen = ex + ell_w;
                for (k = 0; k < nr; k++) {
                    struct lay_rec *r = &c->recs[f->rec_base + k];
                    if (!r->open) {
                        if (r->x1 > f->pen) r->x1 = f->pen;
                        if (r->x0 > f->pen) r->x0 = f->pen;
                    }
                }
            }
        }
    }
    if (nf > 0) {
        struct lay_frag *lf = &fg[nf - 1];
        if (lf->kind == LAY_FRAG_TEXT && lf->font && lf->len > 0 &&
            lf->style && lf->style->letter_spacing != 0) {
            int32_t old_w = lf->w;
            int32_t new_w = text_width_vis(lf->font, lf->style, lf->len);

            if (new_w != old_w) {
                lf->w = new_w;
                for (k = 0; k < nr; k++) {
                    struct lay_rec *r = &c->recs[f->rec_base + k];
                    if (!r->open && r->x1 == lf->x + old_w)
                        r->x1 = lf->x + new_w;
                }
                if (f->pen == lf->x + old_w)
                    f->pen = lf->x + new_w;
            }
        }
    }

    line_w = nf ? fg[nf - 1].x + fg[nf - 1].w : f->pen;
    if (!nf && !nr)
        line_w = 0;

    max_asc = f->strut_asc;
    max_desc = f->strut_desc;
    for (i = 0; i < nf; i++) {
        struct lay_frag *fr = &fg[i];
        int32_t h = fr->asc + fr->desc, above, below;

        switch (fr->valign) {
        case CSS_VALIGN_TOP:
        case CSS_VALIGN_BOTTOM:
            continue;
        case CSS_VALIGN_MIDDLE: {
            int32_t xh = fr->font ? fr->font->xheight
                                  : (fr->style->font_size / 2);

            above = h / 2 + xh / 2;
            below = h - above;
            break;
        }
        case CSS_VALIGN_TEXT_TOP:
            above = f->strut_font_asc;
            below = h - above;
            break;
        case CSS_VALIGN_TEXT_BOTTOM:
            below = f->strut_font_desc;
            above = h - below;
            break;
        default:
            above = fr->asc + fr->shift;
            below = fr->desc - fr->shift;
            break;
        }
        fr->asc = above;
        fr->desc = below;
        if (above > max_asc) max_asc = above;
        if (below > max_desc) max_desc = below;
    }
    for (i = 0; i < nf; i++) {
        struct lay_frag *fr = &fg[i];
        int32_t h = fr->asc + fr->desc;

        if (fr->valign != CSS_VALIGN_TOP && fr->valign != CSS_VALIGN_BOTTOM)
            continue;
        if (max_asc + max_desc < h)
            max_desc = h - max_asc;
    }

    /* text-align */
    justify = f->block->style->text_align == CSS_TEXTALIGN_JUSTIFY &&
              !forced && ifc_avail(f) > line_w;
    switch (f->block->style->text_align) {
    case CSS_TEXTALIGN_RIGHT:
    case CSS_TEXTALIGN_END:
        off = avail - line_w;
        break;
    case CSS_TEXTALIGN_CENTER:
        off = (avail - line_w) / 2;
        break;
    default:
        off = 0;
        break;
    }
    if (off < 0)
        off = 0;
    if (justify) {
        off = 0;
        for (i = 0; i < nf; i++)
            if (fg[i].is_space == 1)
                nsp++;
        extra = nsp ? avail - line_w : 0;
        if (extra < 0)
            extra = 0;
    }

    line = box_new(c, LAY_BOX_LINE, f->block->style, f->block->node);
    if (!line) {
        c->nfrag = f->frag_base;
        return;
    }
    line->flags |= LAYF_ANON;
    line->x = f->line_l + off;
    line->y = f->y;
    line->w = justify && nsp ? avail : line_w;
    line->h = max_asc + max_desc;
    line->baseline = max_asc;
    box_append(f->block, line);
    c->L->stats.line_boxes++;
    f->nlines++;
    if (line->w > f->max_w)
        f->max_w = line->w;
    base_y = line->y + max_asc;

    /* Close any inline box still open, so its extent is known; the
     * boxes themselves are created on demand below, in document order. */
    for (k = 0; k < nr; k++) {
        struct lay_rec *r = &c->recs[f->rec_base + k];

        r->box = 0;
        if (r->open)
            r->x1 = f->pen;
    }

    /* Fragment boxes. */
    {
        int32_t just_add = 0;
        int spaces_seen = 0;

        for (i = 0; i < nf; i++) {
            struct lay_frag *fr = &fg[i];
            struct lay_box *nb;
            struct lay_box *par = ensure_rec(f, fr->rec, line, base_y);
            int32_t fx = line->x + fr->x + just_add;
            int32_t top;
            int32_t h = fr->asc + fr->desc;

            if (justify && fr->is_space == 1 && nsp) {
                int32_t share = extra * (spaces_seen + 1) / nsp -
                                extra * spaces_seen / nsp;

                spaces_seen++;
                just_add += share;
            }
            switch (fr->valign) {
            case CSS_VALIGN_TOP:
                top = line->y;
                break;
            case CSS_VALIGN_BOTTOM:
                top = line->y + line->h - h;
                break;
            default:
                top = base_y - fr->asc;
                break;
            }
            /* `top` is the top of the FRAGMENT box; the glyph cell sits
             * half the leading below it, whatever vertical-align did. */
            if (fr->kind == LAY_FRAG_ATOMIC) {
                lay_rect mb = lay_margin_rect(fr->box);

                box_unlink(fr->box);
                box_append(par, fr->box);
                translate_subtree(fr->box, fx - mb.x, top - mb.y);
                continue;
            }
            (void)0;
            nb = box_new(c, fr->kind == LAY_FRAG_MARKER ? LAY_BOX_MARKER
                                                        : LAY_BOX_TEXT,
                         fr->style, fr->node);
            if (!nb)
                break;
            nb->flags |= LAYF_ANON;
            if (fr->rec >= 0)
                nb->flags |= (c->recs[fr->rec].flags & LAYF_LINK);
            else if (f->block->flags & LAYF_LINK)
                nb->flags |= LAYF_LINK;
            if (fr->style->visibility != CSS_VISIBILITY_VISIBLE)
                nb->flags |= LAYF_HIDDEN;
            nb->text = fr->text;
            nb->text_len = fr->len;
            nb->font = fr->font;
            nb->x = fx;
            nb->w = fr->w;
            if (fr->font) {
                nb->y = top + (fr->asc0 - font_ascent(fr->font));
                nb->h = font_line_height(fr->font);
                nb->baseline = font_ascent(fr->font);
            } else {
                nb->y = top;
                nb->h = h;
                nb->baseline = fr->asc;
            }
            if (fr->kind == LAY_FRAG_MARKER && fr->box)
                nb->marker = fr->box->marker;
            box_append(par, nb);
        }
    }

    /* An inline box with no fragments on this line - <span></span> with
     * padding - still has borders to draw. */
    for (k = 0; k < nr; k++)
        if (!c->recs[f->rec_base + k].box)
            ensure_rec(f, f->rec_base + k, line, base_y);

    f->y += line->h;
    f->first_line = 0;
    c->nfrag = f->frag_base;

    /* Reopen the inline boxes that are still open on the next line. */
    if (f->nopen) {
        int base = c->nrec;
        int n = f->nopen;
        struct lay_rec saved[LAY_MAX_INLINE_NEST];

        for (i = 0; i < n; i++)
            saved[i] = c->recs[f->open[i]];
        c->nrec = f->rec_base = base;
        for (i = 0; i < n; i++) {
            struct lay_rec *r;

            if (c->nrec >= LAY_MAX_FRAGS / 4) {
                c->L->trunc |= LAY_TRUNC_NEST;
                f->nopen = i;
                break;
            }
            r = &c->recs[c->nrec];
            *r = saved[i];
            r->flags &= ~(uint32_t)(LAYF_FIRST_FRAG | LAYF_LAST_FRAG);
            r->open = 1;
            r->box = 0;
            r->x0 = 0;
            r->x1 = 0;
            r->parent = (int16_t)(i ? f->open[i - 1] : -1);
            f->open[i] = (int16_t)c->nrec;
            c->nrec++;
        }
    } else {
        c->nrec = f->rec_base;
    }
}

/* ---- walking the inline content ---------------------------------- */

static void ifc_walk(struct lay_ifc *f, struct lay_box *cont, int depth)
{
    struct lay_ctx *c = f->c;
    struct lay_box *ch, *nx;

    if (depth >= LAY_MAX_DEPTH) {
        c->L->trunc |= LAY_TRUNC_DEPTH;
        return;
    }
    for (ch = cont->first_child; ch; ch = nx) {
        nx = ch->next;   /* captured: committing a line relinks boxes */

        if (ch->flags & LAYF_ABSOLUTE) {
            /* It leaves the flow but not the tree: its containing block
             * is found by walking parents, and the painter reaches it
             * the same way. */
            box_unlink(ch);
            box_append(f->block, ch);
            record_out_of_flow(c, ch, f->line_l + f->pen, f->y);
            continue;
        }
        if (ch->flags & LAYF_FLOAT) {
            int32_t hint = f->y;

            if (ifc_has_content(f) && f->pen > 0)
                hint = f->y + f->strut_asc + f->strut_desc;
            layout_standalone(c, ch, f->line_l, hint, f->cw, 1, depth + 1);
            box_unlink(ch);
            box_append(f->block, ch);
            float_place(c, f->bfc, ch, hint);
            ifc_start_line(f);
            continue;
        }
        if (ch->flags & LAYF_BR) {
            ifc_break(f, 1);
            f->last_forced = 1;
            continue;
        }
        switch (ch->kind) {
        case LAY_BOX_TEXT:
            ifc_add_text(f, ch);
            break;
        case LAY_BOX_MARKER:
            if (ch->style->list_style_position == CSS_LISTPOS_INSIDE)
                ifc_add_marker(f, ch);
            break;
        case LAY_BOX_INLINE:
            if (ifc_open_rec(f, ch) >= 0) {
                ifc_walk(f, ch, depth + 1);
                ifc_close_rec(f);
            }
            break;
        default:
            ifc_add_atomic(f, ch);
            break;
        }
    }
}

/* Lay out the inline content of `block` starting at content_y. Returns
 * the total height of the line boxes produced. */
static int32_t layout_inline(struct lay_ctx *c, struct lay_box *block,
                             struct lay_bfc *bfc, int32_t content_y,
                             int32_t *out_max_w, int depth)
{
    struct lay_ifc f;
    struct lay_box *src = block->first_child;
    const struct font *bf = lay_font_for(block->style);

    memset(&f, 0, sizeof f);
    f.c = c;
    f.block = block;
    f.bfc = bfc;
    f.cx = block->x;
    f.cw = block->w;
    f.y = content_y;
    f.frag_base = c->nfrag;
    f.rec_base = c->nrec;
    f.first_line = 1;
    f.depth = depth;
    half_leading(block->style, bf, &f.strut_asc, &f.strut_desc);
    f.strut_font_asc = font_ascent(bf);
    f.strut_font_desc = font_descent(bf);

    /* Detach the provisional inline children; the walk consumes them and
     * the block is refilled with line boxes. */
    block->first_child = block->last_child = 0;
    {
        struct lay_box *hold = (struct lay_box *)
            lay_arena_alloc(c->L, sizeof *hold);
        struct lay_box *k;

        if (!hold)
            return 0;
        memset(hold, 0, sizeof *hold);
        hold->style = block->style;
        hold->first_child = src;
        /* Reparent, so that unlinking a box during the walk edits the
         * holder rather than the block we are refilling with lines. The
         * holder is arena memory, so the parent pointers left on the
         * boxes we discard stay valid for the life of the layout. */
        for (k = src; k; k = k->next) {
            k->parent = hold;
            hold->last_child = k;
        }
        ifc_start_line(&f);
        ifc_walk(&f, hold, depth + 1);
    }
    while (f.nopen)
        ifc_close_rec(&f);
    if (c->nfrag > f.frag_base || c->nrec > f.rec_base)
        ifc_commit(&f, 0);
    else if (f.last_forced && f.nlines == 0)
        ifc_commit(&f, 1);
    c->nfrag = f.frag_base;
    c->nrec = f.rec_base;

    if (out_max_w)
        *out_max_w = f.max_w;
    if (f.nlines == 0)
        return 0;
    return f.y - content_y;
}

/* ================================================================== *
 * 8  block formatting
 * ================================================================== */

#define LAY_MODE_FLOW   0   /* width from the containing block          */
#define LAY_MODE_SHRINK 1   /* shrink to fit: floats, inline-blocks     */
#define LAY_MODE_FIXEDW 2   /* b->w has already been decided            */

/* layout_block_box() uses about 512 bytes per recursive frame at -O2 and
 * can reach larger inline/table helpers before unwinding. Keep twelve of
 * the public depth slots in reserve: about 6 KiB of headroom on the 64 KiB
 * userspace stack, while the omitted tail still degrades through the
 * normal LAY_TRUNC_DEPTH path. */
#define LAY_BLOCK_DEPTH_HEADROOM 12
#define LAY_BLOCK_MAX_DEPTH (LAY_MAX_DEPTH - LAY_BLOCK_DEPTH_HEADROOM)

static void layout_table(struct lay_ctx *c, struct lay_box *t, int32_t cb_x,
                         int32_t cb_w, int32_t y, int mode, int depth);

static void record_out_of_flow(struct lay_ctx *c, struct lay_box *b,
                               int32_t sx, int32_t sy)
{
    if (c->nposn >= LAY_MAX_POSITIONED) {
        c->L->trunc |= LAY_TRUNC_POSITIONED;
        return;
    }
    b->x = sx;          /* static position, consumed by the abs pass */
    b->y = sy;
    c->posn[c->nposn++] = b;
    c->L->stats.positioned++;
}

/* ---- replaced element sizing ------------------------------------- *
 *
 * CSS 2.1 10.4: with one dimension given and the other auto, the auto
 * one follows from the intrinsic ratio; with both auto, both are
 * intrinsic. An image whose size is not known yet is given a box the
 * size of its alt text so the page does not reflow into nonsense.
 */
static void resolve_replaced(struct lay_ctx *c, struct lay_box *b,
                             int32_t cb_w, int32_t cb_h)
{
    const struct computed_style *cs = b->style;
    int32_t w, h, iw = b->intrinsic_w, ih = b->intrinsic_h;
    int32_t h_bp, v_bp;
    int auto_w = len_is_auto_or_indef(cs->width, cb_w);
    int auto_h = len_is_auto_or_indef(cs->height, cb_h);

    b->pad[CSS_TOP]    = used_len(cs->padding[CSS_TOP], cb_w, 0);
    b->pad[CSS_RIGHT]  = used_len(cs->padding[CSS_RIGHT], cb_w, 0);
    b->pad[CSS_BOTTOM] = used_len(cs->padding[CSS_BOTTOM], cb_w, 0);
    b->pad[CSS_LEFT]   = used_len(cs->padding[CSS_LEFT], cb_w, 0);
    b->bord[CSS_TOP]    = cs->border_width[CSS_TOP];
    b->bord[CSS_RIGHT]  = cs->border_width[CSS_RIGHT];
    b->bord[CSS_BOTTOM] = cs->border_width[CSS_BOTTOM];
    b->bord[CSS_LEFT]   = cs->border_width[CSS_LEFT];
    b->marg[CSS_TOP]    = used_len(cs->margin[CSS_TOP], cb_w, 0);
    b->marg[CSS_RIGHT]  = used_len(cs->margin[CSS_RIGHT], cb_w, 0);
    b->marg[CSS_BOTTOM] = used_len(cs->margin[CSS_BOTTOM], cb_w, 0);
    b->marg[CSS_LEFT]   = used_len(cs->margin[CSS_LEFT], cb_w, 0);
    h_bp = box_h_bp(b);
    v_bp = box_v_bp(b);

    if (!iw && !ih && b->node) {
        /* No decoder and no attributes: size the placeholder from the
         * alt text, which is what the painter will draw. */
        const char *alt = dom_get_attr(b->node, "alt");
        const struct font *f = lay_font_for(cs);

        if (alt && *alt) {
            unsigned long n = strlen(alt);

            if (n > 200) n = 200;
            iw = text_width(f, alt, n) + 8;
            ih = font_line_height(f) + 4;
        } else if (!((cs->width.type == CSS_LEN_PX ||
                      (cs->width.type == CSS_LEN_CALC && cs->width.pct == 0)) ||
                     (cs->height.type == CSS_LEN_PX ||
                      (cs->height.type == CSS_LEN_CALC && cs->height.pct == 0)))) {
            iw = 0;
            ih = 0;
        }
    }

    w = auto_w ? -1 : content_dim(used_len(cs->width, cb_w, 0), cs, h_bp);
    h = auto_h ? -1 : content_dim(used_len(cs->height, cb_h, 0), cs, v_bp);

    if (w < 0 && h < 0) {
        if (cs->aspect_ratio > 0) {
            if (iw > 0) {
                w = iw;
                h = ratio_height_from_width(cs, w, h_bp, v_bp);
            } else if (ih > 0) {
                h = ih;
                w = ratio_width_from_height(cs, h, h_bp, v_bp);
            } else {
                w = 0;
                h = 0;
            }
        } else {
            w = iw;
            h = ih;
        }
    } else if (w < 0) {
        if (cs->aspect_ratio > 0)
            w = ratio_width_from_height(cs, h, h_bp, v_bp);
        else
            w = (ih > 0) ? clamp32((long long)h * iw / ih) : iw;
    } else if (h < 0) {
        if (cs->aspect_ratio > 0)
            h = ratio_height_from_width(cs, w, h_bp, v_bp);
        else
            h = (iw > 0) ? clamp32((long long)w * ih / iw) : ih;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    {
        int32_t cw = clamp_width(w, cs, cb_w, h_bp);
        int32_t chh;

        if (cw != w && auto_h) {
            if (cs->aspect_ratio > 0)
                h = ratio_height_from_width(cs, cw, h_bp, v_bp);
            else if (iw > 0)
                h = clamp32((long long)cw * ih / iw);
        }
        w = cw;
        chh = clamp_height(h, cs, cb_h, v_bp);
        if (chh != h && auto_w) {
            if (cs->aspect_ratio > 0)
                w = clamp_width(ratio_width_from_height(cs, chh, h_bp, v_bp),
                                cs, cb_w, h_bp);
            else if (ih > 0)
                w = clamp_width(clamp32((long long)chh * iw / ih),
                                cs, cb_w, h_bp);
        }
        h = chh;
    }
    b->w = w;
    b->h = h;
    b->baseline = h + b->pad[CSS_BOTTOM] + b->bord[CSS_BOTTOM];
    (void)c;
}

/* ---- horizontal resolution --------------------------------------- */

static void resolve_edges(struct lay_box *b, int32_t cb_w)
{
    const struct computed_style *cs = b->style;

    b->pad[CSS_TOP]    = used_len(cs->padding[CSS_TOP], cb_w, 0);
    b->pad[CSS_RIGHT]  = used_len(cs->padding[CSS_RIGHT], cb_w, 0);
    b->pad[CSS_BOTTOM] = used_len(cs->padding[CSS_BOTTOM], cb_w, 0);
    b->pad[CSS_LEFT]   = used_len(cs->padding[CSS_LEFT], cb_w, 0);
    b->bord[CSS_TOP]    = cs->border_width[CSS_TOP];
    b->bord[CSS_RIGHT]  = cs->border_width[CSS_RIGHT];
    b->bord[CSS_BOTTOM] = cs->border_width[CSS_BOTTOM];
    b->bord[CSS_LEFT]   = cs->border_width[CSS_LEFT];
    b->marg[CSS_TOP]    = used_len(cs->margin[CSS_TOP], cb_w, 0);
    b->marg[CSS_BOTTOM] = used_len(cs->margin[CSS_BOTTOM], cb_w, 0);
}

/* CSS 2.1 10.3.3. `spec_w` < 0 means the width is auto. */
static void solve_width(struct lay_box *b, int32_t cb_w, int32_t spec_w)
{
    const struct computed_style *cs = b->style;
    int auto_l = len_is_auto(cs->margin[CSS_LEFT]);
    int auto_r = len_is_auto(cs->margin[CSS_RIGHT]);
    int32_t ml = auto_l ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
    int32_t mr = auto_r ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);
    int32_t edges = b->bord[CSS_LEFT] + b->pad[CSS_LEFT] +
                    b->bord[CSS_RIGHT] + b->pad[CSS_RIGHT];
    int32_t rest;

    if (spec_w < 0) {
        b->w = imax(0, cb_w - edges - ml - mr);
        b->marg[CSS_LEFT] = ml;
        b->marg[CSS_RIGHT] = mr;
        return;
    }
    b->w = spec_w;
    rest = cb_w - edges - spec_w;
    if (auto_l && auto_r) {
        int32_t half = rest / 2;

        if (rest < 0) half = 0;
        ml = half;
        mr = rest - half;
        if (mr < 0 && rest >= 0) mr = 0;
    } else if (auto_l) {
        ml = rest - mr;
        if (ml < 0) ml = 0;
    } else if (auto_r) {
        mr = rest - ml;
    } else {
        /* Over-constrained: the right margin is the one that gives. */
        mr = rest - ml;
    }
    b->marg[CSS_LEFT] = ml;
    b->marg[CSS_RIGHT] = mr;
}

static void resolve_horizontal(struct lay_ctx *c, struct lay_box *b,
                               int32_t cb_w, int32_t cb_h, int mode, int depth)
{
    const struct computed_style *cs = b->style;
    int32_t spec, clamped, h_bp;

    resolve_edges(b, cb_w);
    h_bp = box_h_bp(b);

    if (mode == LAY_MODE_FIXEDW) {
        b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
            ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
        b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
            ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);
        b->w = clamp_width(b->w, cs, cb_w, h_bp);
        return;
    }
    if (mode == LAY_MODE_SHRINK) {
        int32_t edges = h_bp;

        b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
            ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
        b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
            ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);
        if (len_is_auto_or_indef(cs->width, cb_w)) {
            if (cs->aspect_ratio > 0 && !len_is_auto_or_indef(cs->height, cb_h)) {
                int32_t vbp = box_v_bp(b);
                int32_t dh = clamp_height(
                    content_dim(used_len(cs->height, cb_h, 0), cs, vbp),
                    cs, cb_h, vbp);
                b->w = ratio_width_from_height(cs, dh, h_bp, vbp);
            } else {
                int32_t mn = 0, mx = 0, avail;

                intrinsic_widths(c, b, &mn, &mx, depth);
                mn -= edges + b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];
                mx -= edges + b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];
                if (mn < 0) mn = 0;
                if (mx < mn) mx = mn;
                avail = cb_w - edges - b->marg[CSS_LEFT] - b->marg[CSS_RIGHT];
                b->w = imin(imax(mn, avail), mx);
            }
        } else {
            b->w = content_dim(used_len(cs->width, cb_w, 0), cs, h_bp);
        }
        b->w = clamp_width(b->w, cs, cb_w, h_bp);
        return;
    }

    if (len_is_auto_or_indef(cs->width, cb_w)) {
        if (cs->aspect_ratio > 0 && !len_is_auto_or_indef(cs->height, cb_h)) {
            int32_t vbp = box_v_bp(b);
            int32_t dh = clamp_height(
                content_dim(used_len(cs->height, cb_h, 0), cs, vbp),
                cs, cb_h, vbp);
            spec = ratio_width_from_height(cs, dh, h_bp, vbp);
        } else {
            spec = -1;
        }
    } else {
        spec = content_dim(used_len(cs->width, cb_w, 0), cs, h_bp);
    }
    solve_width(b, cb_w, spec);
    clamped = clamp_width(b->w, cs, cb_w, h_bp);
    if (clamped != b->w)
        solve_width(b, cb_w, clamped);   /* CSS 2.1 10.4: redo with it */
}

/* ---- collapsed top margin ---------------------------------------- *
 *
 * The top margin of a block collapses with the top margins of its first
 * in-flow children as long as nothing - a border, padding, a line box, a
 * new formatting context - separates them. Walking that chain here, up
 * front, is what lets a box be positioned in one pass instead of being
 * laid out and then shifted.
 */
static int block_collapses_with_children(const struct lay_box *b)
{
    return b->kind == LAY_BOX_BLOCK &&
           !(b->flags & (LAYF_BFC | LAYF_FLOAT | LAYF_ABSOLUTE | LAYF_IFC)) &&
           !b->bord[CSS_TOP] && !b->pad[CSS_TOP];
}

static struct lay_marg collapsed_top(struct lay_box *b, int32_t cb_w)
{
    struct lay_marg m = marg_of(len_is_auto(b->style->margin[CSS_TOP])
                                ? 0
                                : used_len(b->style->margin[CSS_TOP], cb_w, 0));
    int n = 0;

    while (n++ < LAY_MAX_ADJOIN) {
        struct lay_box *ch;
        const struct computed_style *cs = b->style;

        if (b->kind != LAY_BOX_BLOCK)
            break;
        if (b->flags & (LAYF_BFC | LAYF_FLOAT | LAYF_ABSOLUTE | LAYF_IFC))
            break;
        if (cs->border_width[CSS_TOP] || used_len(cs->padding[CSS_TOP], cb_w, 0))
            break;
        for (ch = b->first_child; ch; ch = ch->next) {
            if (ch->kind == LAY_BOX_MARKER)
                continue;
            if (ch->flags & (LAYF_FLOAT | LAYF_ABSOLUTE))
                continue;
            break;
        }
        if (!ch)
            break;
        m = marg_collapse(m, marg_of(len_is_auto(ch->style->margin[CSS_TOP])
                                     ? 0
                                     : used_len(ch->style->margin[CSS_TOP],
                                                cb_w, 0)));
        b = ch;
    }
    return m;
}

/* ---- list markers ------------------------------------------------ */

static int32_t marker_width(const struct lay_box *m)
{
    const struct font *f = m->font;

    if (m->marker)
        return text_width(f, m->marker, (unsigned long)strlen(m->marker)) +
               f->w / 2;
    return f->w + f->w / 2;
}

static void place_marker_outside(struct lay_box *b, struct lay_box *m)
{
    struct lay_box *n;
    int32_t base = b->y + font_ascent(m->font);
    int guard = 0;

    for (n = b; n && guard++ < LAY_MAX_DEPTH; ) {
        if (n->kind == LAY_BOX_LINE) {
            base = n->y + n->baseline;
            break;
        }
        n = n->first_child;
    }
    m->w = marker_width(m);
    m->h = font_line_height(m->font);
    m->x = b->x - m->w;
    m->y = base - font_ascent(m->font);
    m->baseline = font_ascent(m->font);
    box_append(b, m);
}

struct flex_item {
    struct lay_box *box;
    int32_t basis;
    int32_t allocated;
    int32_t min_main;
    int32_t inner_basis;
};

static int32_t flex_col_gap(const struct computed_style *cs)
{
    if (cs->column_gap > 0) return cs->column_gap;
    if (cs->gap > 0) return cs->gap;
    return 0;
}

static int32_t flex_row_gap(const struct computed_style *cs)
{
    if (cs->row_gap > 0) return cs->row_gap;
    if (cs->gap > 0) return cs->gap;
    return 0;
}

/* Distance from margin-box top of `b` to its first in-flow line baseline
 * (or bottom margin edge when there are no lines). */
static int32_t item_first_baseline_offset(struct lay_box *b)
{
    lay_rect mb = lay_margin_rect(b);
    struct lay_box *n = b;
    int guard = 0;

    if (b->kind == LAY_BOX_REPLACED || (b->flags & LAYF_CLIP))
        return mb.h;
    for (;;) {
        if (guard++ > 200000)
            break;
        if (n->kind == LAY_BOX_LINE)
            return n->y + n->baseline - mb.y;
        if (n->first_child) { n = n->first_child; continue; }
        while (n != b && !n->next) n = n->parent;
        if (n == b) break;
        n = n->next;
    }
    return mb.h;
}

static void flex_resolve_line(struct flex_item *items, int s, int e,
                              int32_t avail_main, int32_t main_gap,
                              int32_t *out_free)
{
    int n_line = e - s, i;
    int64_t total = (int64_t)main_gap * (n_line > 0 ? n_line - 1 : 0);
    int64_t grow_total = 0;

    for (i = s; i < e; i++) {
        items[i].allocated = items[i].basis;
        total += items[i].basis;
        grow_total += items[i].box->style->flex_grow;
    }
    if (total < avail_main) {
        int32_t free_space = (int32_t)(avail_main - total);

        if (grow_total > 0) {
            int32_t remaining = free_space;
            int64_t grow_left = grow_total;

            for (i = s; i < e; i++) {
                int32_t grow = items[i].box->style->flex_grow;
                int32_t add;

                if (grow <= 0)
                    continue;
                add = grow == grow_left ? remaining :
                    (int32_t)((int64_t)remaining * grow / grow_left);
                items[i].allocated += add;
                remaining -= add;
                grow_left -= grow;
            }
            *out_free = 0;
        } else {
            *out_free = free_space;
        }
        return;
    }
    *out_free = 0;
    if (total > avail_main) {
        int32_t over = (int32_t)(total - avail_main);
        int pass;

        for (pass = 0; pass < n_line && over > 0; pass++) {
            int64_t wsum = 0;
            int32_t shrunk = 0;

            for (i = s; i < e; i++) {
                int32_t sh = items[i].box->style->flex_shrink;
                int32_t room = items[i].allocated - items[i].min_main;
                int32_t wt = items[i].inner_basis > 0
                    ? items[i].inner_basis : items[i].allocated;

                if (sh > 0 && room > 0 && wt > 0)
                    wsum += (int64_t)wt * sh;
            }
            if (wsum <= 0)
                break;
            {
                int32_t rem = over;
                int64_t wleft = wsum;

                for (i = s; i < e && rem > 0; i++) {
                    int32_t sh = items[i].box->style->flex_shrink;
                    int32_t room = items[i].allocated - items[i].min_main;
                    int32_t wt = items[i].inner_basis > 0
                        ? items[i].inner_basis : items[i].allocated;
                    int64_t w = (sh > 0 && room > 0 && wt > 0)
                        ? (int64_t)wt * sh : 0;
                    int32_t cut;

                    if (w <= 0)
                        continue;
                    cut = w == wleft ? rem : (int32_t)((int64_t)rem * w / wleft);
                    if (cut > room)
                        cut = room;
                    items[i].allocated -= cut;
                    shrunk += cut;
                    rem -= cut;
                    wleft -= w;
                }
            }
            if (shrunk <= 0)
                break;
            over -= shrunk;
        }
    }
}

static void flex_justify(int justify, int reverse, int n_line,
                         int32_t free_space, int32_t *start,
                         int32_t *gap_extra)
{
    *start = 0;
    *gap_extra = 0;
    if (free_space <= 0)
        return;
    if (reverse) {
        if (justify == CSS_JUSTIFY_START) justify = CSS_JUSTIFY_END;
        else if (justify == CSS_JUSTIFY_END) justify = CSS_JUSTIFY_START;
    }
    switch (justify) {
    case CSS_JUSTIFY_END:
        *start = free_space;
        break;
    case CSS_JUSTIFY_CENTER:
        *start = free_space / 2;
        break;
    case CSS_JUSTIFY_SPACE_BETWEEN:
        if (n_line > 1) *gap_extra = free_space / (n_line - 1);
        break;
    case CSS_JUSTIFY_SPACE_AROUND:
        *gap_extra = free_space / n_line;
        *start = *gap_extra / 2;
        break;
    case CSS_JUSTIFY_SPACE_EVENLY:
        *gap_extra = free_space / (n_line + 1);
        *start = *gap_extra;
        break;
    default:
        break;
    }
}

static void layout_item_fixedw(struct lay_ctx *c, struct lay_box *ch,
                               int32_t x, int32_t y, int32_t alloc_outer_w,
                               int32_t cb_w, int32_t cb_h, int force_w,
                               int depth)
{
    const struct computed_style *ics = ch->style;
    struct lay_marg open = marg_of(0);
    struct lay_bfc bfc;
    int32_t ml, mr, inner_w;

    ch->flags |= LAYF_BFC;
    resolve_edges(ch, cb_w);
    ml = len_is_auto(ics->margin[CSS_LEFT]) ? 0
                                            : used_len(ics->margin[CSS_LEFT], cb_w, 0);
    mr = len_is_auto(ics->margin[CSS_RIGHT]) ? 0
                                             : used_len(ics->margin[CSS_RIGHT], cb_w, 0);
    ch->marg[CSS_LEFT] = ml;
    ch->marg[CSS_RIGHT] = mr;
    inner_w = imax(0, alloc_outer_w - box_h_bp(ch) - ml - mr);

    if (ch->kind == LAY_BOX_REPLACED) {
        resolve_replaced(c, ch, cb_w, cb_h);
        if (force_w) {
            inner_w = clamp_width(inner_w, ics, cb_w, box_h_bp(ch));
            if (inner_w != ch->w) {
                if (len_is_auto_or_indef(ics->height, cb_h)) {
                    if (ics->aspect_ratio > 0) {
                        ch->h = clamp_height(
                            ratio_height_from_width(ics, inner_w,
                                                    box_h_bp(ch), box_v_bp(ch)),
                            ics, cb_h, box_v_bp(ch));
                    } else if (ch->intrinsic_w > 0 && ch->intrinsic_h > 0) {
                        ch->h = clamp_height(
                            clamp32((long long)inner_w * ch->intrinsic_h /
                                    ch->intrinsic_w),
                            ics, cb_h, box_v_bp(ch));
                    }
                }
                ch->w = inner_w;
                ch->baseline = ch->h + ch->pad[CSS_BOTTOM] +
                               ch->bord[CSS_BOTTOM];
            }
        }
        ch->x = x + ml + ch->bord[CSS_LEFT] + ch->pad[CSS_LEFT];
        ch->y = y + ch->marg[CSS_TOP] + ch->bord[CSS_TOP] + ch->pad[CSS_TOP];
        return;
    }
    if (ch->kind == LAY_BOX_TABLE) {
        ch->w = inner_w;
        layout_table(c, ch, x, alloc_outer_w, y + ch->marg[CSS_TOP],
                     force_w ? LAY_MODE_FIXEDW : LAY_MODE_FLOW, depth + 1);
        return;
    }
    ch->w = inner_w;
    bfc.base = c->nfl;
    bfc.cx = x;
    bfc.cw = alloc_outer_w;
    bfc.low_left = bfc.low_right = 0;
    layout_block_box(c, ch, x, cb_w, cb_h, y, &bfc, &open, 0,
                     LAY_MODE_FIXEDW, depth + 1);
    c->nfl = bfc.base;
}

#if defined(__GNUC__) || defined(__clang__)
#define LAY_NOINLINE __attribute__((noinline))
#else
#define LAY_NOINLINE
#endif

static int effective_align(const struct computed_style *parent,
                           const struct computed_style *item)
{
    if (item && item->align_self != CSS_ALIGNSELF_AUTO &&
        item->align_self < CSS_ALIGNSELF_COUNT)
        return (int)item->align_self - 1;
    return parent->align_items;
}

static void sort_flex_items(struct flex_item *items, int n)
{
    int i, j;

    for (i = 1; i < n; i++) {
        struct flex_item key = items[i];
        int32_t ko = key.box->style->order;

        j = i - 1;
        while (j >= 0 && items[j].box->style->order > ko) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }
}

static LAY_NOINLINE int32_t layout_flex_row(struct lay_ctx *c, struct lay_box *b,
                                            int32_t y, int32_t definite_h,
                                            struct lay_bfc *bfc, int depth);
static LAY_NOINLINE int32_t layout_flex_column(struct lay_ctx *c,
                                               struct lay_box *b,
                                               int32_t y, int32_t definite_h,
                                               struct lay_bfc *bfc, int depth);
static LAY_NOINLINE int32_t layout_grid(struct lay_ctx *c, struct lay_box *b,
                                        int32_t y, int32_t definite_h,
                                        struct lay_bfc *bfc, int depth);

static int32_t layout_flex_row(struct lay_ctx *c, struct lay_box *b,
                               int32_t y, int32_t definite_h,
                               struct lay_bfc *bfc, int depth)
{
    struct flex_item *items;
    int *line_starts, *line_hs;
    struct lay_box *ch;
    int count = 0, i, k, reverse, wrap, wrap_rev, nlines = 0;
    int32_t main_gap = flex_col_gap(b->style);
    int32_t cross_gap = flex_row_gap(b->style);
    int32_t total_h = 0, cur_y;

    for (ch = b->first_child; ch; ch = ch->next)
        if (ch->kind != LAY_BOX_MARKER && !(ch->flags & LAYF_ABSOLUTE))
            count++;
    if (!count)
        return definite_h >= 0 ? definite_h : 0;
    if (count > 512)
        return -1;
    items = (struct flex_item *)calloc((unsigned long)count, sizeof(*items));
    line_starts = (int *)calloc((unsigned long)(count + 1), sizeof(int));
    line_hs = (int *)calloc((unsigned long)count, sizeof(int));
    if (!items || !line_starts || !line_hs) {
        free(items); free(line_starts); free(line_hs);
        return -1;
    }

    i = 0;
    for (ch = b->first_child; ch; ch = ch->next) {
        const struct computed_style *ics = ch->style;
        int32_t ml, mr, h_bp, h_extra, mn = 0, mx = 0, basis, inner_b;

        if (ch->kind == LAY_BOX_MARKER)
            continue;
        if (ch->flags & LAYF_ABSOLUTE) {
            record_out_of_flow(c, ch, b->x, y);
            continue;
        }
        resolve_edges(ch, b->w);
        ml = len_is_auto(ics->margin[CSS_LEFT]) ? 0
                                                : used_len(ics->margin[CSS_LEFT], b->w, 0);
        mr = len_is_auto(ics->margin[CSS_RIGHT]) ? 0
                                                 : used_len(ics->margin[CSS_RIGHT], b->w, 0);
        h_bp = box_h_bp(ch);
        h_extra = h_bp + ml + mr;

        if (!len_is_auto_or_indef(ics->flex_basis, b->w)) {
            inner_b = content_dim(used_len(ics->flex_basis, b->w, 0), ics, h_bp);
            inner_b = clamp_width(inner_b, ics, b->w, h_bp);
            basis = inner_b + h_extra;
        } else if (!len_is_auto_or_indef(ics->width, b->w)) {
            inner_b = content_dim(used_len(ics->width, b->w, 0), ics, h_bp);
            inner_b = clamp_width(inner_b, ics, b->w, h_bp);
            basis = inner_b + h_extra;
        } else if (ics->aspect_ratio > 0 &&
                   !len_is_auto_or_indef(ics->height, definite_h)) {
            int32_t vbp = box_v_bp(ch);
            int32_t dh = clamp_height(
                content_dim(used_len(ics->height, definite_h, 0), ics, vbp),
                ics, definite_h, vbp);
            inner_b = clamp_width(ratio_width_from_height(ics, dh, h_bp, vbp),
                                  ics, b->w, h_bp);
            basis = inner_b + h_extra;
        } else {
            intrinsic_widths(c, ch, &mn, &mx, depth + 1);
            basis = mx;
            inner_b = imax(0, basis - h_extra);
        }
        if (basis < h_extra) basis = h_extra;
        items[i].box = ch;
        items[i].basis = basis;
        items[i].allocated = basis;
        items[i].inner_basis = inner_b;
        items[i].min_main = h_extra +
            (!len_is_auto_or_indef(ics->min_width, b->w)
             ? content_dim(used_len(ics->min_width, b->w, 0), ics, h_bp) : 0);
        if (items[i].min_main > items[i].basis)
            items[i].min_main = items[i].basis;
        i++;
    }

    sort_flex_items(items, count);

    wrap = b->style->flex_wrap == CSS_FLEXWRAP_WRAP ||
           b->style->flex_wrap == CSS_FLEXWRAP_WRAP_REVERSE;
    wrap_rev = b->style->flex_wrap == CSS_FLEXWRAP_WRAP_REVERSE;
    if (!wrap) {
        nlines = 1;
        line_starts[0] = 0;
        line_starts[1] = count;
    } else {
        int s = 0;
        while (s < count) {
            int64_t sum = items[s].basis;
            int e = s + 1;

            while (e < count && sum + main_gap + items[e].basis <= b->w) {
                sum += main_gap + items[e].basis;
                e++;
            }
            line_starts[nlines++] = s;
            s = e;
        }
        line_starts[nlines] = count;
    }

    reverse = b->style->flex_direction == CSS_FLEXDIR_ROW_REVERSE;
    for (k = 0; k < nlines; k++) {
        int s = line_starts[k], e = line_starts[k + 1], n_line = e - s, j;
        int32_t free_space = 0, start = 0, gap_extra = 0, cursor;
        int32_t line_h = 0, line_cross, max_asc = 0, max_desc = 0;
        int have_base = 0;

        flex_resolve_line(items, s, e, b->w, main_gap, &free_space);
        flex_justify(b->style->justify_content, reverse, n_line, free_space,
                     &start, &gap_extra);

        cursor = b->x + start;
        for (j = 0; j < n_line; j++) {
            int at = reverse ? e - 1 - j : s + j;
            lay_rect r;

            ch = items[at].box;
            layout_item_fixedw(c, ch, cursor, 0, items[at].allocated,
                               b->w, definite_h, 1, depth);
            r = lay_margin_rect(ch);
            if (r.h > line_h)
                line_h = r.h;
            if (effective_align(b->style, ch->style) == CSS_ALIGN_BASELINE) {
                int32_t asc = item_first_baseline_offset(ch);
                int32_t desc = r.h - asc;

                have_base = 1;
                if (asc > max_asc) max_asc = asc;
                if (desc > max_desc) max_desc = desc;
            }
            cursor += items[at].allocated + main_gap + gap_extra;
        }
        if (have_base && max_asc + max_desc > line_h)
            line_h = max_asc + max_desc;
        line_cross = (nlines == 1 && definite_h >= 0 && definite_h > line_h)
            ? definite_h : line_h;
        for (j = s; j < e; j++) {
            lay_rect r = lay_margin_rect(items[j].box);
            int32_t dy = 0;
            int al;

            ch = items[j].box;
            al = effective_align(b->style, ch->style);
            switch (al) {
            case CSS_ALIGN_END:
                dy = line_cross - r.h;
                break;
            case CSS_ALIGN_CENTER:
                dy = (line_cross - r.h) / 2;
                break;
            case CSS_ALIGN_BASELINE:
                dy = max_asc - item_first_baseline_offset(ch);
                break;
            case CSS_ALIGN_STRETCH:
                if (len_is_auto_or_indef(ch->style->height, definite_h) &&
                    ch->kind != LAY_BOX_REPLACED && r.h < line_cross) {
                    int32_t want = imax(0, line_cross - ch->marg[CSS_TOP] -
                                        ch->marg[CSS_BOTTOM] - box_v_bp(ch));
                    want = clamp_height(want, ch->style, definite_h,
                                        box_v_bp(ch));
                    if (want > ch->h)
                        ch->h = want;
                }
                break;
            default:
                break;
            }
            if (dy > 0)
                translate_subtree(ch, 0, dy);
        }
        line_hs[k] = line_cross;
        total_h += line_cross;
    }
    if (nlines > 1)
        total_h += cross_gap * (nlines - 1);

    cur_y = y;
    if (wrap_rev && definite_h >= 0 && definite_h > total_h)
        cur_y += definite_h - total_h;
    for (k = 0; k < nlines; k++) {
        int lk = wrap_rev ? nlines - 1 - k : k;
        int s = line_starts[lk], e = line_starts[lk + 1], j;

        for (j = s; j < e; j++)
            translate_subtree(items[j].box, 0, cur_y);
        cur_y += line_hs[lk] + cross_gap;
    }

    free(items);
    free(line_starts);
    free(line_hs);
    (void)bfc;
    return definite_h >= 0 ? imax(definite_h, total_h) : total_h;
}

static int32_t layout_flex_column(struct lay_ctx *c, struct lay_box *b,
                                  int32_t y, int32_t definite_h,
                                  struct lay_bfc *bfc, int depth)
{
    struct flex_item *items;
    int *line_starts, *line_ws;
    struct lay_box *ch;
    int count = 0, i, k, reverse, wrap, wrap_rev, nlines = 0;
    int32_t main_gap = flex_row_gap(b->style);
    int32_t cross_gap = flex_col_gap(b->style);
    int32_t max_col_h = 0, cur_x;

    for (ch = b->first_child; ch; ch = ch->next)
        if (ch->kind != LAY_BOX_MARKER && !(ch->flags & LAYF_ABSOLUTE))
            count++;
    if (!count)
        return definite_h >= 0 ? definite_h : 0;
    if (count > 512)
        return -1;
    items = (struct flex_item *)calloc((unsigned long)count, sizeof(*items));
    line_starts = (int *)calloc((unsigned long)(count + 1), sizeof(int));
    line_ws = (int *)calloc((unsigned long)count, sizeof(int));
    if (!items || !line_starts || !line_ws) {
        free(items); free(line_starts); free(line_ws);
        return -1;
    }

    wrap = (b->style->flex_wrap == CSS_FLEXWRAP_WRAP ||
            b->style->flex_wrap == CSS_FLEXWRAP_WRAP_REVERSE) &&
           definite_h >= 0;
    wrap_rev = b->style->flex_wrap == CSS_FLEXWRAP_WRAP_REVERSE;

    i = 0;
    for (ch = b->first_child; ch; ch = ch->next) {
        const struct computed_style *ics = ch->style;
        lay_rect r;
        int32_t v_bp, v_marg, basis, inner_b;
        int shrink_w;

        if (ch->kind == LAY_BOX_MARKER)
            continue;
        if (ch->flags & LAYF_ABSOLUTE) {
            record_out_of_flow(c, ch, b->x, y);
            continue;
        }
        ch->flags |= LAYF_BFC;
        shrink_w = wrap ||
                   effective_align(b->style, ics) != CSS_ALIGN_STRETCH ||
                   !len_is_auto_or_indef(ics->width, b->w);
        layout_standalone(c, ch, b->x, 0, b->w, shrink_w, depth + 1);
        r = lay_margin_rect(ch);
        v_bp = box_v_bp(ch);
        v_marg = ch->marg[CSS_TOP] + ch->marg[CSS_BOTTOM];

        if (!len_is_auto_or_indef(ics->flex_basis, definite_h)) {
            inner_b = content_dim(used_len(ics->flex_basis, definite_h, 0),
                                  ics, v_bp);
            inner_b = clamp_height(inner_b, ics, definite_h, v_bp);
            ch->h = inner_b;
            basis = inner_b + v_bp + v_marg;
        } else if (!len_is_auto_or_indef(ics->height, definite_h)) {
            inner_b = content_dim(used_len(ics->height, definite_h, 0),
                                  ics, v_bp);
            inner_b = clamp_height(inner_b, ics, definite_h, v_bp);
            ch->h = inner_b;
            basis = inner_b + v_bp + v_marg;
        } else {
            basis = r.h > 0 ? r.h : 0;
            inner_b = imax(0, basis - v_bp - v_marg);
        }
        if (basis < v_bp + v_marg)
            basis = v_bp + v_marg;
        items[i].box = ch;
        items[i].basis = basis;
        items[i].allocated = basis;
        items[i].inner_basis = inner_b;
        items[i].min_main = v_bp + v_marg +
            (!len_is_auto_or_indef(ics->min_height, definite_h)
             ? content_dim(used_len(ics->min_height, definite_h, 0), ics, v_bp)
             : 0);
        if (items[i].min_main > items[i].basis)
            items[i].min_main = items[i].basis;
        i++;
    }

    sort_flex_items(items, count);

    if (!wrap) {
        nlines = 1;
        line_starts[0] = 0;
        line_starts[1] = count;
    } else {
        int s = 0;
        while (s < count) {
            int64_t sum = items[s].basis;
            int e = s + 1;

            while (e < count && sum + main_gap + items[e].basis <= definite_h) {
                sum += main_gap + items[e].basis;
                e++;
            }
            line_starts[nlines++] = s;
            s = e;
        }
        line_starts[nlines] = count;
    }

    reverse = b->style->flex_direction == CSS_FLEXDIR_COLUMN_REVERSE;
    for (k = 0; k < nlines; k++) {
        int s = line_starts[k], e = line_starts[k + 1], n_line = e - s, j;
        int64_t col_sum = (int64_t)main_gap * (n_line > 0 ? n_line - 1 : 0);
        int32_t col_avail, free_space = 0, start = 0, gap_extra = 0, cursor;
        int32_t col_w = 0, col_cross;

        for (j = s; j < e; j++) {
            lay_rect mr = lay_margin_rect(items[j].box);
            col_sum += items[j].basis;
            if (mr.w > col_w)
                col_w = mr.w;
        }
        col_avail = definite_h >= 0 ? definite_h :
            (col_sum > 2147483647LL ? 2147483647 : (int32_t)col_sum);
        if (col_avail > max_col_h)
            max_col_h = col_avail;

        flex_resolve_line(items, s, e, col_avail, main_gap, &free_space);
        for (j = s; j < e; j++) {
            ch = items[j].box;
            if (items[j].allocated != items[j].basis) {
                int32_t nh = imax(0, items[j].allocated - box_v_bp(ch) -
                                  ch->marg[CSS_TOP] - ch->marg[CSS_BOTTOM]);
                if (ch->kind != LAY_BOX_REPLACED) {
                    ch->h = nh;
                    if (ch->style->aspect_ratio > 0 &&
                        len_is_auto_or_indef(ch->style->width, b->w)) {
                        ch->w = clamp_width(
                            ratio_width_from_height(ch->style, nh,
                                                    box_h_bp(ch), box_v_bp(ch)),
                            ch->style, b->w, box_h_bp(ch));
                    }
                }
            }
        }
        flex_justify(b->style->justify_content, reverse, n_line, free_space,
                     &start, &gap_extra);

        col_cross = nlines == 1 ? b->w : col_w;
        cursor = y + start;
        for (j = 0; j < n_line; j++) {
            int at = reverse ? e - 1 - j : s + j;
            lay_rect r = lay_margin_rect(items[at].box);
            lay_rect border = lay_border_rect(items[at].box);
            int32_t ix = b->x;
            int al;

            ch = items[at].box;
            al = effective_align(b->style, ch->style);
            if (al == CSS_ALIGN_END)
                ix += col_cross - border.w;
            else if (al == CSS_ALIGN_CENTER)
                ix += (col_cross - border.w) / 2;
            if (ix < b->x)
                ix = b->x;
            translate_subtree(ch, ix - border.x, cursor - r.y);
            cursor += items[at].allocated + main_gap + gap_extra;
        }
        line_ws[k] = col_cross;
    }

    if (nlines > 1) {
        cur_x = 0;
        for (k = 0; k < nlines; k++) {
            int lk = wrap_rev ? nlines - 1 - k : k;
            int s = line_starts[lk], e = line_starts[lk + 1], j;

            for (j = s; j < e; j++)
                translate_subtree(items[j].box, cur_x, 0);
            cur_x += line_ws[lk] + cross_gap;
        }
    }

    free(items);
    free(line_starts);
    free(line_ws);
    (void)bfc;
    return definite_h >= 0 ? definite_h : max_col_h;
}

/* ---- CSS Grid ---------------------------------------------------- */

struct grid_item {
    struct lay_box *box;
    int row, col, rspan, cspan;
    int r0, c0; /* explicit 0-based start, or -1 if auto */
};

static void sort_grid_items(struct grid_item *items, int n)
{
    int i, j;

    for (i = 1; i < n; i++) {
        struct grid_item key = items[i];
        int32_t ko = key.box->style->order;

        j = i - 1;
        while (j >= 0 && items[j].box->style->order > ko) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }
}

static void grid_size_rows(const struct computed_style *cs, int nrows,
                           int32_t definite_h, int32_t row_gap,
                           int32_t *row_h)
{
    int r;
    int32_t fixed_h = 0, total_gaps = row_gap * (nrows - 1);
    int64_t fr_total = 0, max_fr_unit = 0;

    if (cs->grid_nrows == 0 || nrows <= 0)
        return;
    for (r = 0; r < nrows; r++) {
        struct css_grid_track tr;

        if (r < cs->grid_nrows) {
            tr = cs->grid_rows[r];
        } else {
            tr.kind = CSS_GRID_AUTO;
            tr.val = 0;
        }
        if (tr.kind == CSS_GRID_NONE)
            tr.kind = CSS_GRID_AUTO;
        if (tr.kind == CSS_GRID_PX) {
            row_h[r] = imax(0, tr.val);
            fixed_h += row_h[r];
        } else if (tr.kind == CSS_GRID_PCT) {
            if (definite_h >= 0)
                row_h[r] = imax(0, clamp32((int64_t)definite_h * tr.val / 100000));
            fixed_h += row_h[r];
        } else if (tr.kind == CSS_GRID_FR) {
            if (tr.val > 0) {
                int64_t unit = ((int64_t)row_h[r] * 1000LL + tr.val - 1) / tr.val;
                if (unit > max_fr_unit)
                    max_fr_unit = unit;
                fr_total += tr.val;
            } else {
                row_h[r] = 0;
            }
        } else {
            fixed_h += row_h[r];
        }
    }
    if (fr_total > 0) {
        if (definite_h >= 0) {
            int32_t rem = definite_h - total_gaps - fixed_h;
            int32_t left = rem > 0 ? rem : 0;
            int64_t fr_left = fr_total;

            for (r = 0; r < nrows; r++) {
                if (r >= cs->grid_nrows || cs->grid_rows[r].kind != CSS_GRID_FR ||
                    cs->grid_rows[r].val <= 0)
                    continue;
                row_h[r] = cs->grid_rows[r].val == fr_left ? left :
                    (int32_t)((int64_t)left * cs->grid_rows[r].val / fr_left);
                left -= row_h[r];
                fr_left -= cs->grid_rows[r].val;
            }
        } else {
            for (r = 0; r < nrows; r++) {
                int32_t h;
                if (r >= cs->grid_nrows || cs->grid_rows[r].kind != CSS_GRID_FR ||
                    cs->grid_rows[r].val <= 0)
                    continue;
                h = (int32_t)((max_fr_unit * cs->grid_rows[r].val + 500) / 1000);
                if (h > row_h[r])
                    row_h[r] = h;
            }
        }
    }
}

static int grid_occ_fits(const uint8_t *occ, int occ_rows, int ncols,
                         int r, int c, int rspan, int cspan)
{
    int rr, cc;

    if (c < 0 || c + cspan > ncols)
        return 0;
    for (rr = r; rr < r + rspan; rr++) {
        if (rr >= occ_rows)
            continue;
        for (cc = c; cc < c + cspan; cc++)
            if (occ[rr * ncols + cc])
                return 0;
    }
    return 1;
}

static int grid_occ_mark(uint8_t **occ, int *occ_rows, int ncols,
                         int r, int c, int rspan, int cspan)
{
    int need = r + rspan, rr, cc;

    if (need > *occ_rows) {
        int new_rows = *occ_rows ? *occ_rows : 8;
        uint8_t *nb;

        while (new_rows < need) {
            if (new_rows > 2048) return 0;
            new_rows *= 2;
        }
        nb = (uint8_t *)realloc(*occ, (unsigned long)new_rows * ncols);
        if (!nb)
            return 0;
        memset(nb + (unsigned long)(*occ_rows) * ncols, 0,
               (unsigned long)(new_rows - *occ_rows) * ncols);
        *occ = nb;
        *occ_rows = new_rows;
    }
    for (rr = r; rr < r + rspan; rr++)
        for (cc = c; cc < c + cspan; cc++)
            (*occ)[rr * ncols + cc] = 1;
    return 1;
}

static int32_t layout_grid(struct lay_ctx *c, struct lay_box *b,
                           int32_t y, int32_t definite_h,
                           struct lay_bfc *bfc, int depth)
{
    const struct computed_style *cs = b->style;
    int ncols = cs->grid_ncols > 0
        ? (cs->grid_ncols > CSS_GRID_MAX_COLS ? CSS_GRID_MAX_COLS : cs->grid_ncols)
        : 1;
    int32_t col_gap = flex_col_gap(cs);
    int32_t row_gap = flex_row_gap(cs);
    struct grid_item *items;
    struct lay_box *ch;
    int count = 0, i, nrows = 0, occ_rows = 0, cur_r = 0, cur_c = 0;
    uint8_t *occ = 0;
    struct css_grid_track tracks[CSS_GRID_MAX_COLS];
    int32_t col_w[CSS_GRID_MAX_COLS], col_min[CSS_GRID_MAX_COLS];
    int32_t col_max[CSS_GRID_MAX_COLS], col_x[CSS_GRID_MAX_COLS];
    int32_t *row_h = 0, *row_y = 0, *row_asc = 0, *row_desc = 0;
    int32_t fixed_w = 0, total_col_gaps, avail_w, rem_w, total_h = 0;
    int64_t fr_total = 0;

    for (ch = b->first_child; ch; ch = ch->next)
        if (ch->kind != LAY_BOX_MARKER && !(ch->flags & LAYF_ABSOLUTE))
            count++;
    if (!count)
        return definite_h >= 0 ? definite_h : 0;
    if (count > 2048)
        return -1;
    items = (struct grid_item *)calloc((unsigned long)count, sizeof(*items));
    if (!items)
        return -1;

    for (i = 0; i < ncols; i++) {
        if (cs->grid_ncols > 0)
            tracks[i] = cs->grid_cols[i];
        else {
            tracks[i].kind = CSS_GRID_FR;
            tracks[i].val = 1000;
        }
        if (tracks[i].kind == CSS_GRID_NONE)
            tracks[i].kind = CSS_GRID_AUTO;
        col_w[i] = col_min[i] = col_max[i] = 0;
    }

    i = 0;
    for (ch = b->first_child; ch; ch = ch->next) {
        const struct computed_style *ics = ch->style;
        int c0, r0, cspan, rspan;

        if (ch->kind == LAY_BOX_MARKER)
            continue;
        if (ch->flags & LAYF_ABSOLUTE) {
            record_out_of_flow(c, ch, b->x, y);
            continue;
        }
        ch->flags |= LAYF_BFC;

        if (ics->grid_col_start > 0 && ics->grid_col_end > ics->grid_col_start)
            cspan = ics->grid_col_end - ics->grid_col_start;
        else
            cspan = ics->grid_col_span >= 1 ? ics->grid_col_span : 1;
        if (cspan > ncols) cspan = ncols;
        if (ics->grid_col_start > 0) {
            c0 = ics->grid_col_start - 1;
            if (c0 >= ncols) { c0 = ncols - 1; cspan = 1; }
            if (c0 + cspan > ncols) cspan = ncols - c0;
        } else if (ics->grid_col_end > 0) {
            c0 = imax(0, ics->grid_col_end - 1 - cspan);
        } else {
            c0 = -1;
        }

        if (ics->grid_row_start > 0 && ics->grid_row_end > ics->grid_row_start)
            rspan = ics->grid_row_end - ics->grid_row_start;
        else
            rspan = ics->grid_row_span >= 1 ? ics->grid_row_span : 1;
        if (rspan > 64) rspan = 64;
        if (ics->grid_row_start > 0) {
            r0 = ics->grid_row_start - 1;
            if (r0 > 2048) r0 = 2048;
        } else if (ics->grid_row_end > 0) {
            r0 = imax(0, ics->grid_row_end - 1 - rspan);
        } else {
            r0 = -1;
        }

        items[i].box = ch;
        items[i].c0 = c0;
        items[i].r0 = r0;
        items[i].cspan = cspan;
        items[i].rspan = rspan;
        items[i].row = 0;
        items[i].col = 0;
        i++;
    }

    sort_grid_items(items, count);

    /* Placement pass 1a: explicit row AND explicit col. */
    for (i = 0; i < count; i++) {
        if (items[i].r0 < 0 || items[i].c0 < 0)
            continue;
        items[i].row = items[i].r0;
        items[i].col = items[i].c0;
        grid_occ_mark(&occ, &occ_rows, ncols, items[i].row,
                      items[i].col, items[i].rspan, items[i].cspan);
    }
    /* Placement pass 1b: explicit row, auto col. */
    for (i = 0; i < count; i++) {
        int r = items[i].r0, cc = 0, placed = 0;

        if (items[i].r0 < 0 || items[i].c0 >= 0)
            continue;
        while (!placed && r < 4096) {
            for (cc = 0; cc <= ncols - items[i].cspan; cc++) {
                if (grid_occ_fits(occ, occ_rows, ncols, r, cc,
                                  items[i].rspan, items[i].cspan)) {
                    items[i].row = r;
                    items[i].col = cc;
                    grid_occ_mark(&occ, &occ_rows, ncols, r, cc,
                                  items[i].rspan, items[i].cspan);
                    placed = 1;
                    break;
                }
            }
            r++;
        }
    }
    /* Placement pass 2: remaining items (auto row) in sparse DOM order. */
    for (i = 0; i < count; i++) {
        int placed = 0;

        if (items[i].r0 >= 0)
            continue;
        if (items[i].c0 >= 0) {
            if (items[i].c0 < cur_c) {
                cur_c = 0;
                cur_r++;
            }
            while (cur_r < 4096 &&
                   !grid_occ_fits(occ, occ_rows, ncols, cur_r, items[i].c0,
                                  items[i].rspan, items[i].cspan))
                cur_r++;
            if (cur_r < 4096) {
                items[i].row = cur_r;
                items[i].col = items[i].c0;
                grid_occ_mark(&occ, &occ_rows, ncols, cur_r, items[i].c0,
                              items[i].rspan, items[i].cspan);
                cur_c = items[i].c0 + items[i].cspan;
                if (cur_c >= ncols) { cur_c = 0; cur_r++; }
            }
            continue;
        }
        while (!placed && cur_r < 4096) {
            for (; cur_c <= ncols - items[i].cspan; cur_c++) {
                if (grid_occ_fits(occ, occ_rows, ncols, cur_r, cur_c,
                                  items[i].rspan, items[i].cspan)) {
                    items[i].row = cur_r;
                    items[i].col = cur_c;
                    grid_occ_mark(&occ, &occ_rows, ncols, cur_r, cur_c,
                                  items[i].rspan, items[i].cspan);
                    cur_c += items[i].cspan;
                    if (cur_c >= ncols) { cur_c = 0; cur_r++; }
                    placed = 1;
                    break;
                }
            }
            if (!placed) { cur_c = 0; cur_r++; }
        }
    }
    free(occ);
    occ = 0;

    for (i = 0; i < count; i++)
        if (items[i].row + items[i].rspan > nrows)
            nrows = items[i].row + items[i].rspan;
    if (cs->grid_nrows > nrows)
        nrows = cs->grid_nrows;
    if (nrows <= 0) {
        free(items);
        return definite_h >= 0 ? definite_h : 0;
    }

    /* Track width sizing */
    avail_w = b->w;
    total_col_gaps = col_gap * (ncols - 1);
    for (i = 0; i < ncols; i++) {
        if (tracks[i].kind == CSS_GRID_PX)
            col_w[i] = imax(0, tracks[i].val);
        else if (tracks[i].kind == CSS_GRID_PCT)
            col_w[i] = imax(0, clamp32((int64_t)avail_w * tracks[i].val / 100000));
    }
    for (i = 0; i < count; i++) {
        int32_t mn = 0, mx = 0;
        if (items[i].cspan != 1)
            continue;
        if (tracks[items[i].col].kind != CSS_GRID_AUTO)
            continue;
        intrinsic_widths(c, items[i].box, &mn, &mx, depth + 1);
        if (mn > col_min[items[i].col]) col_min[items[i].col] = mn;
        if (mx > col_max[items[i].col]) col_max[items[i].col] = mx;
    }
    for (i = 0; i < ncols; i++) {
        if (tracks[i].kind == CSS_GRID_AUTO)
            col_w[i] = col_max[i];
    }
    /* If auto tracks overflow available width, shrink them down toward min */
    {
        int32_t non_fr_sum = 0, auto_span = 0;
        for (i = 0; i < ncols; i++) {
            if (tracks[i].kind != CSS_GRID_FR)
                non_fr_sum += col_w[i];
            if (tracks[i].kind == CSS_GRID_AUTO)
                auto_span += col_max[i] - col_min[i];
        }
        if (non_fr_sum + total_col_gaps > avail_w && auto_span > 0) {
            int32_t over = non_fr_sum + total_col_gaps - avail_w;
            if (over > auto_span) over = auto_span;
            for (i = 0; i < ncols; i++) {
                if (tracks[i].kind == CSS_GRID_AUTO &&
                    col_max[i] > col_min[i]) {
                    int32_t d = (int32_t)((int64_t)over *
                                          (col_max[i] - col_min[i]) / auto_span);
                    col_w[i] -= d;
                }
            }
        }
    }
    for (i = 0; i < ncols; i++) {
        if (tracks[i].kind == CSS_GRID_FR) {
            if (tracks[i].val > 0)
                fr_total += tracks[i].val;
        } else {
            fixed_w += col_w[i];
        }
    }
    rem_w = avail_w - total_col_gaps - fixed_w;
    if (rem_w < 0) rem_w = 0;
    if (fr_total > 0 && rem_w > 0) {
        int32_t left = rem_w;
        int64_t fr_left = fr_total;

        for (i = 0; i < ncols; i++) {
            if (tracks[i].kind != CSS_GRID_FR || tracks[i].val <= 0)
                continue;
            col_w[i] = tracks[i].val == fr_left ? left :
                (int32_t)((int64_t)left * tracks[i].val / fr_left);
            left -= col_w[i];
            fr_left -= tracks[i].val;
        }
    }

    col_x[0] = b->x;
    for (i = 1; i < ncols; i++)
        col_x[i] = col_x[i - 1] + col_w[i - 1] + col_gap;

    row_h = (int32_t *)calloc((unsigned long)nrows, sizeof(int32_t));
    row_y = (int32_t *)calloc((unsigned long)nrows, sizeof(int32_t));
    row_asc = (int32_t *)calloc((unsigned long)nrows, sizeof(int32_t));
    row_desc = (int32_t *)calloc((unsigned long)nrows, sizeof(int32_t));
    if (!row_h || !row_y || !row_asc || !row_desc) {
        free(items); free(row_h); free(row_y); free(row_asc); free(row_desc);
        return -1;
    }

    /* Lay out every grid item inside its spanned track width */
    for (i = 0; i < count; i++) {
        struct lay_box *item = items[i].box;
        const struct computed_style *ics = item->style;
        int32_t area_w = col_gap * (items[i].cspan - 1);
        int32_t ml, mr, h_bp, item_outer_w, want_inner_w;
        int k;

        for (k = 0; k < items[i].cspan; k++)
            area_w += col_w[items[i].col + k];
        resolve_edges(item, area_w);
        ml = len_is_auto(ics->margin[CSS_LEFT]) ? 0
                                                : used_len(ics->margin[CSS_LEFT], area_w, 0);
        mr = len_is_auto(ics->margin[CSS_RIGHT]) ? 0
                                                 : used_len(ics->margin[CSS_RIGHT], area_w, 0);
        h_bp = box_h_bp(item);
        if (!len_is_auto_or_indef(ics->width, area_w)) {
            want_inner_w = content_dim(used_len(ics->width, area_w, 0), ics, h_bp);
            want_inner_w = clamp_width(want_inner_w, ics, area_w, h_bp);
            item_outer_w = want_inner_w + h_bp + ml + mr;
            layout_item_fixedw(c, item, col_x[items[i].col], 0, item_outer_w,
                               area_w, definite_h, 1, depth);
        } else {
            want_inner_w = imax(0, area_w - h_bp - ml - mr);
            want_inner_w = clamp_width(want_inner_w, ics, area_w, h_bp);
            item_outer_w = want_inner_w + h_bp + ml + mr;
            layout_item_fixedw(c, item, col_x[items[i].col], 0, item_outer_w,
                               area_w, definite_h, 0, depth);
        }
    }

    /* Measure row heights: single-row spans first, then multi-row spans */
    {
        int have_base = 0;

        for (i = 0; i < count; i++) {
            lay_rect r = lay_margin_rect(items[i].box);

            if (items[i].rspan != 1)
                continue;
            if (r.h > row_h[items[i].row])
                row_h[items[i].row] = r.h;
            if (effective_align(cs, items[i].box->style) == CSS_ALIGN_BASELINE) {
                int32_t asc = item_first_baseline_offset(items[i].box);
                int32_t desc = r.h - asc;
                have_base = 1;
                if (asc > row_asc[items[i].row]) row_asc[items[i].row] = asc;
                if (desc > row_desc[items[i].row]) row_desc[items[i].row] = desc;
            }
        }
        if (have_base) {
            for (i = 0; i < nrows; i++)
                if (row_asc[i] + row_desc[i] > row_h[i])
                    row_h[i] = row_asc[i] + row_desc[i];
        }
    }
    for (i = 0; i < count; i++) {
        int32_t have = row_gap * (items[i].rspan - 1), want;
        int k;

        if (items[i].rspan <= 1)
            continue;
        for (k = 0; k < items[i].rspan; k++)
            have += row_h[items[i].row + k];
        want = lay_margin_rect(items[i].box).h;
        if (want > have)
            row_h[items[i].row + items[i].rspan - 1] += want - have;
    }

    grid_size_rows(cs, nrows, definite_h, row_gap, row_h);

    row_y[0] = y;
    for (i = 1; i < nrows; i++)
        row_y[i] = row_y[i - 1] + row_h[i - 1] + row_gap;
    total_h = row_y[nrows - 1] + row_h[nrows - 1] - y;

    /* Position and vertically align/stretch items into their cells */
    for (i = 0; i < count; i++) {
        struct lay_box *item = items[i].box;
        const struct computed_style *ics = item->style;
        lay_rect r = lay_margin_rect(item);
        int32_t span_h = row_gap * (items[i].rspan - 1);
        int32_t dy = row_y[items[i].row] - r.y;
        int k, al;

        for (k = 0; k < items[i].rspan; k++)
            span_h += row_h[items[i].row + k];

        if (ics->height.type == CSS_LEN_PCT && definite_h < 0 &&
            items[i].row < cs->grid_nrows && item->kind != LAY_BOX_REPLACED) {
            int32_t vbp = box_v_bp(item);
            int32_t nh = clamp_height(
                content_dim(used_len(ics->height, span_h, 0), ics, vbp),
                ics, span_h, vbp);
            item->h = nh;
            r = lay_margin_rect(item);
        }

        al = effective_align(cs, ics);
        switch (al) {
        case CSS_ALIGN_END:
            dy += span_h - r.h;
            break;
        case CSS_ALIGN_CENTER:
            dy += (span_h - r.h) / 2;
            break;
        case CSS_ALIGN_BASELINE:
            if (items[i].rspan == 1)
                dy += row_asc[items[i].row] - item_first_baseline_offset(item);
            break;
        case CSS_ALIGN_STRETCH:
            if ((len_is_auto(ics->height) ||
                 (len_is_auto_or_indef(ics->height, definite_h) &&
                  items[i].row >= cs->grid_nrows)) &&
                item->kind != LAY_BOX_REPLACED && r.h < span_h) {
                int32_t want = imax(0, span_h - item->marg[CSS_TOP] -
                                    item->marg[CSS_BOTTOM] - box_v_bp(item));
                int32_t hbase = definite_h >= 0 ? definite_h : span_h;
                want = clamp_height(want, ics, hbase, box_v_bp(item));
                if (want > item->h)
                    item->h = want;
            }
            break;
        default:
            break;
        }
        if (dy)
            translate_subtree(item, 0, dy);
    }

    free(items);
    free(row_h);
    free(row_y);
    free(row_asc);
    free(row_desc);
    (void)bfc;
    return total_h;
}

/* ---- the block box ----------------------------------------------- */

static int32_t layout_block_box(struct lay_ctx *c, struct lay_box *b,
                                int32_t cb_x, int32_t cb_w, int32_t cb_h,
                                int32_t y, struct lay_bfc *bfc,
                                struct lay_marg *io_open, int skip_top,
                                int mode, int depth)
{
    const struct computed_style *cs = b->style;
    struct lay_marg open_in = *io_open, ctop, open, mbot, outgoing;
    struct lay_bfc mine;
    struct lay_box *ch, *nx, *marker = 0;
    int32_t btop, content_y, flow, h_children = 0, used_h, bb_h;
    int32_t definite_h = -1;
    int first = 1, has_lines = 0, can_skip, auto_h, through = 0;
    char here;

    if ((uintptr_t)&here < c->stack_lo)
        c->stack_lo = (uintptr_t)&here;
    if ((uintptr_t)&here > c->stack_hi)
        c->stack_hi = (uintptr_t)&here;
    if (depth > c->depth_max)
        c->depth_max = depth;
    if (depth >= LAY_BLOCK_MAX_DEPTH) {
        c->L->trunc |= LAY_TRUNC_DEPTH;
        *io_open = open_in;
        return y;
    }

    if (b->kind == LAY_BOX_TABLE) {
        int32_t applied;

        ctop = marg_of(len_is_auto(cs->margin[CSS_TOP])
                       ? 0 : used_len(cs->margin[CSS_TOP], cb_w, 0));
        applied = skip_top ? 0 : marg_value(marg_collapse(open_in, ctop));
        btop = clear_past(c, bfc, cs->clear, y + applied);
        layout_table(c, b, cb_x, cb_w, btop, mode, depth);
        *io_open = marg_of(used_len(cs->margin[CSS_BOTTOM], cb_w, 0));
        return btop + lay_border_rect(b).h;
    }
    if (b->kind == LAY_BOX_REPLACED) {
        int32_t applied;

        resolve_replaced(c, b, cb_w, cb_h);
        ctop = marg_of(b->marg[CSS_TOP]);
        applied = skip_top ? 0 : marg_value(marg_collapse(open_in, ctop));
        btop = clear_past(c, bfc, cs->clear, y + applied);
        if (len_is_auto(cs->margin[CSS_LEFT]) &&
            len_is_auto(cs->margin[CSS_RIGHT])) {
            int32_t rest = cb_w - lay_border_rect(b).w;

            if (rest > 0) {
                b->marg[CSS_LEFT] = rest / 2;
                b->marg[CSS_RIGHT] = rest - rest / 2;
            }
        }
        b->x = cb_x + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT];
        b->y = btop + b->bord[CSS_TOP] + b->pad[CSS_TOP];
        *io_open = marg_of(b->marg[CSS_BOTTOM]);
        return btop + lay_border_rect(b).h;
    }

    resolve_horizontal(c, b, cb_w, cb_h, mode, depth);
    auto_h = len_is_auto_or_indef(cs->height, cb_h);
    if (!auto_h) {
        definite_h = content_dim(used_len(cs->height, cb_h, 0), cs,
                                 box_v_bp(b));
        definite_h = clamp_height(definite_h, cs, cb_h, box_v_bp(b));
        b->h = definite_h;
    }
    ctop = skip_top ? marg_of(0) : collapsed_top(b, cb_w);
    btop = y + (skip_top ? 0 : marg_value(marg_collapse(open_in, ctop)));
    btop = clear_past(c, bfc, cs->clear, btop);

    b->x = cb_x + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT];

    /* CSS 2.1 9.5: a box that establishes a new block formatting context
     * must not overlap a float in its parent context, so it narrows and
     * shifts instead. Evaluated in the band at its top edge. */
    if ((b->flags & LAYF_BFC) && mode == LAY_MODE_FLOW &&
        !(b->flags & (LAYF_FLOAT | LAYF_ABSOLUTE)) && c->nfl > bfc->base) {
        int32_t l, r;

        line_edges(c, bfc, btop, 1, cb_x, cb_x + cb_w, &l, &r);
        if (l > cb_x || r < cb_x + cb_w) {
            int32_t avail = r - l;
            int32_t edges = box_h_bp(b) +
                            b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];

            if (len_is_auto(cs->width))
                b->w = clamp_width(imax(0, avail - edges), cs, cb_w,
                                   box_h_bp(b));
            b->x = l + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT];
        }
    }

    {
        int32_t ar_h = -1;
        int32_t pass_h;

        if (auto_h && cs->aspect_ratio > 0) {
            ar_h = ratio_height_from_width(cs, b->w, box_h_bp(b), box_v_bp(b));
            ar_h = clamp_height(ar_h, cs, cb_h, box_v_bp(b));
        }
        pass_h = auto_h ? ar_h : definite_h;

        b->y = btop + b->bord[CSS_TOP] + b->pad[CSS_TOP];
        content_y = b->y;
        flow = content_y;

        mine.base = (b->flags & LAYF_BFC) ? c->nfl : bfc->base;
        mine.cx = b->x;
        mine.cw = b->w;
        mine.low_left = mine.low_right = 0;

        if (b->first_child && b->first_child->kind == LAY_BOX_MARKER &&
            b->first_child->style->list_style_position == CSS_LISTPOS_OUTSIDE) {
            marker = b->first_child;
            box_unlink(marker);
        }

        if (cs->display == CSS_DISPLAY_FLEX ||
            cs->display == CSS_DISPLAY_INLINE_FLEX) {
            if (cs->flex_direction == CSS_FLEXDIR_ROW ||
                cs->flex_direction == CSS_FLEXDIR_ROW_REVERSE)
                h_children = layout_flex_row(c, b, content_y, pass_h,
                                             &mine, depth);
            else
                h_children = layout_flex_column(c, b, content_y, pass_h,
                                                &mine, depth);
            if (h_children >= 0) {
                flow = content_y + h_children;
                open = marg_of(0);
            } else {
                /* Allocation failure retains the readable block fallback. */
                open = marg_of(0);
                can_skip = 0;
                for (ch = b->first_child; ch; ch = nx) {
                    nx = ch->next;
                    if (ch->kind == LAY_BOX_MARKER)
                        continue;
                    if (ch->flags & LAYF_ABSOLUTE) {
                        record_out_of_flow(c, ch, b->x,
                                           flow + marg_value(open));
                        continue;
                    }
                    flow = layout_block_box(c, ch, b->x, b->w, pass_h,
                                            flow, &mine, &open, 0,
                                            LAY_MODE_FLOW, depth + 1);
                }
                h_children = flow - content_y;
            }
        } else if (cs->display == CSS_DISPLAY_GRID ||
                   cs->display == CSS_DISPLAY_INLINE_GRID) {
            h_children = layout_grid(c, b, content_y, pass_h, &mine, depth);
            if (h_children >= 0) {
                flow = content_y + h_children;
                open = marg_of(0);
            } else {
                open = marg_of(0);
                can_skip = 0;
                for (ch = b->first_child; ch; ch = nx) {
                    nx = ch->next;
                    if (ch->kind == LAY_BOX_MARKER)
                        continue;
                    if (ch->flags & LAYF_ABSOLUTE) {
                        record_out_of_flow(c, ch, b->x,
                                           flow + marg_value(open));
                        continue;
                    }
                    flow = layout_block_box(c, ch, b->x, b->w, pass_h,
                                            flow, &mine, &open, 0,
                                            LAY_MODE_FLOW, depth + 1);
                }
                h_children = flow - content_y;
            }
        } else if (b->flags & LAYF_IFC) {
            int32_t mw = 0;

            h_children = layout_inline(c, b, &mine, content_y, &mw, depth);
            has_lines = h_children > 0;
            flow = content_y + h_children;
            open = marg_of(0);
        } else {
            open = marg_of(0);
            can_skip = block_collapses_with_children(b) && !skip_top ? 1 :
                       block_collapses_with_children(b);
            for (ch = b->first_child; ch; ch = nx) {
                nx = ch->next;
                if (ch->kind == LAY_BOX_MARKER)
                    continue;
                if (ch->flags & LAYF_ABSOLUTE) {
                    record_out_of_flow(c, ch, b->x, flow + marg_value(open));
                    continue;
                }
                if (ch->flags & LAYF_FLOAT) {
                    int32_t hint = flow + marg_value(open);

                    layout_standalone(c, ch, b->x, hint, b->w, 1, depth + 1);
                    float_place(c, &mine, ch, hint);
                    continue;
                }
                flow = layout_block_box(c, ch, b->x, b->w, pass_h,
                                        flow, &mine, &open,
                                        first && can_skip, LAY_MODE_FLOW,
                                        depth + 1);
                first = 0;
            }
            h_children = flow - content_y;
        }

        mbot = marg_of(len_is_auto(cs->margin[CSS_BOTTOM])
                       ? 0 : used_len(cs->margin[CSS_BOTTOM], cb_w, 0));
        b->marg[CSS_TOP] = len_is_auto(cs->margin[CSS_TOP])
            ? 0 : used_len(cs->margin[CSS_TOP], cb_w, 0);
        b->marg[CSS_BOTTOM] = mbot.pos + mbot.neg;

        if (!auto_h) {
            used_h = definite_h;
            outgoing = mbot;
        } else if (b->flags & LAYF_BFC) {
            int32_t fl = float_lowest(c, &mine);

            used_h = h_children + marg_value(open);
            if (fl - content_y > used_h)
                used_h = fl - content_y;
            outgoing = mbot;
        } else if (b->bord[CSS_BOTTOM] || b->pad[CSS_BOTTOM]) {
            used_h = h_children + marg_value(open);
            outgoing = mbot;
        } else {
            used_h = h_children;
            outgoing = marg_collapse(open, mbot);
        }
        if (auto_h && ar_h >= 0) {
            if (b->flags & LAYF_CLIP)
                used_h = ar_h;
            else if (ar_h > used_h)
                used_h = ar_h;
        }
        used_h = clamp_height(used_h, cs, cb_h, box_v_bp(b));
        b->h = used_h;
    }

    if (auto_h && used_h == 0 && !has_lines &&
        !b->bord[CSS_TOP] && !b->pad[CSS_TOP] &&
        !b->bord[CSS_BOTTOM] && !b->pad[CSS_BOTTOM] &&
        !(b->flags & LAYF_BFC))
        through = 1;

    if (b->flags & LAYF_BFC)
        c->nfl = mine.base;

    if (marker)
        place_marker_outside(b, marker);

    bb_h = b->bord[CSS_TOP] + b->pad[CSS_TOP] + b->h +
           b->pad[CSS_BOTTOM] + b->bord[CSS_BOTTOM];

    if (through) {
        *io_open = marg_collapse(marg_collapse(open_in, ctop),
                                 marg_collapse(open, mbot));
        return y;
    }
    *io_open = outgoing;
    return btop + bb_h;
}

static void layout_standalone(struct lay_ctx *c, struct lay_box *b,
                              int32_t x, int32_t y, int32_t avail_w,
                              int shrink, int depth)
{
    struct lay_marg open = marg_of(0);
    struct lay_bfc bfc;

    if (avail_w < 0)
        avail_w = 0;
    bfc.base = c->nfl;
    bfc.cx = x;
    bfc.cw = avail_w;
    bfc.low_left = bfc.low_right = 0;

    if (b->kind == LAY_BOX_REPLACED) {
        resolve_replaced(c, b, avail_w, -1);
        b->x = x + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT];
        b->y = y + b->marg[CSS_TOP] + b->bord[CSS_TOP] + b->pad[CSS_TOP];
        return;
    }
    layout_block_box(c, b, x, avail_w, -1, y, &bfc, &open, 0,
                     shrink ? LAY_MODE_SHRINK : LAY_MODE_FLOW, depth);
    c->nfl = bfc.base;
}

/* ================================================================== *
 * 9  tables
 *
 * Automatic layout measures every cell's minimum and maximum content
 * width, folds them into per-column figures (spanning cells only after
 * the single-span ones have had their say), then distributes the
 * available width between the two. Fixed layout takes the widths the
 * first row declares and shares what is left equally. Row heights come
 * from a second pass so that a rowspan can lengthen the last row it
 * touches without invalidating the rows above it.
 * ================================================================== */

struct tcol {
    int32_t minw, maxw, w;
    int32_t spec;      /* fixed-layout declared width, or -1 */
};

static int32_t table_spacing(const struct lay_box *t)
{
    if (t->style->border_collapse == CSS_BORDERCOLLAPSE_COLLAPSE)
        return 0;
    return t->style->border_spacing > 0 ? t->style->border_spacing : 0;
}

/* Assign every cell a grid slot, honouring rowspan occupancy. Returns
 * the column count. */
static int table_build_grid(struct lay_ctx *c, struct lay_box *t, int *nrows)
{
    int32_t *occ;
    struct lay_box *g, *r, *rnext, *cell;
    int ncols = 0, rows = 0, i;

    occ = (int32_t *)lay_arena_alloc(c->L, sizeof(int32_t) * LAY_MAX_COLUMNS);
    if (!occ) {
        *nrows = 0;
        return 0;
    }
    memset(occ, 0, sizeof(int32_t) * LAY_MAX_COLUMNS);

    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = rnext) {
            int col = 0;

            rnext = r->next;
            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            if (rows >= LAY_MAX_ROWS ||
                (unsigned long)rows > (unsigned long)UINT16_MAX) {
                c->L->trunc |= LAY_TRUNC_TABLE;
                box_unlink(r);
                continue;
            }
            r->row = (uint16_t)rows;
            for (cell = r->first_child; cell; cell = cell->next) {
                int span, k;

                if (cell->kind != LAY_BOX_TABLE_CELL)
                    continue;
                while (col < LAY_MAX_COLUMNS && occ[col] > 0)
                    col++;
                if (col >= LAY_MAX_COLUMNS) {
                    c->L->trunc |= LAY_TRUNC_TABLE;
                    break;
                }
                span = cell->colspan;
                if (col + span > LAY_MAX_COLUMNS)
                    span = LAY_MAX_COLUMNS - col;
                cell->col = (uint16_t)col;
                cell->row = (uint16_t)rows;
                cell->colspan = (uint16_t)(span < 1 ? 1 : span);
                for (k = 0; k < span; k++)
                    occ[col + k] = cell->rowspan < 1 ? 1 : cell->rowspan;
                col += span;
                if (col > ncols)
                    ncols = col;
            }
            for (i = 0; i < LAY_MAX_COLUMNS; i++)
                if (occ[i] > 0)
                    occ[i]--;
            rows++;
        }
    }
    *nrows = rows;
    return ncols;
}

/* Column minimum and maximum content widths. */
static struct tcol *table_measure(struct lay_ctx *c, struct lay_box *t,
                                  int ncols, int depth)
{
    struct tcol *col;
    struct lay_box *g, *r, *cell;
    int i;

    if (ncols <= 0)
        return 0;
    col = (struct tcol *)lay_arena_alloc(c->L, sizeof(struct tcol) * ncols);
    if (!col)
        return 0;
    for (i = 0; i < ncols; i++) {
        col[i].minw = col[i].maxw = col[i].w = 0;
        col[i].spec = -1;
    }
    /* single-span cells first */
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next)
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t mn = 0, mx = 0, px = 0;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->colspan != 1)
                    continue;
                if (cell->col >= ncols)
                    continue;
                intrinsic_widths(c, cell, &mn, &mx, depth + 1);
                if (mn > col[cell->col].minw) col[cell->col].minw = mn;
                if (mx > col[cell->col].maxw) col[cell->col].maxw = mx;
                if (len_is_fixed_px(cell->style->width, &px)) {
                    int32_t spec = cell->style->box_sizing == CSS_BOXSIZING_BORDER
                        ? px : (px + hedge_bp(cell->style));
                    if (spec > col[cell->col].spec)
                        col[cell->col].spec = spec;
                }
            }
    }
    for (i = 0; i < ncols; i++)
        if (col[i].maxw < col[i].minw)
            col[i].maxw = col[i].minw;

    /* spanning cells distribute whatever the columns they cover lack */
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next)
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t mn = 0, mx = 0, sum_min = 0, sum_max = 0, def;
                int k, n;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->colspan <= 1)
                    continue;
                n = cell->colspan;
                if (cell->col + n > ncols)
                    n = ncols - cell->col;
                if (n <= 0)
                    continue;
                intrinsic_widths(c, cell, &mn, &mx, depth + 1);
                for (k = 0; k < n; k++) {
                    sum_min += col[cell->col + k].minw;
                    sum_max += col[cell->col + k].maxw;
                }
                def = mn - sum_min;
                if (def > 0) {
                    for (k = 0; k < n; k++) {
                        int32_t share = sum_min > 0
                            ? clamp32((long long)def *
                                      col[cell->col + k].minw / sum_min)
                            : def / n;

                        col[cell->col + k].minw += share;
                    }
                    /* rounding: the last column takes the remainder */
                    sum_min = 0;
                    for (k = 0; k < n; k++)
                        sum_min += col[cell->col + k].minw;
                    if (sum_min < mn)
                        col[cell->col + n - 1].minw += mn - sum_min;
                }
                def = mx - sum_max;
                if (def > 0) {
                    for (k = 0; k < n; k++) {
                        int32_t share = sum_max > 0
                            ? clamp32((long long)def *
                                      col[cell->col + k].maxw / sum_max)
                            : def / n;

                        col[cell->col + k].maxw += share;
                    }
                    sum_max = 0;
                    for (k = 0; k < n; k++)
                        sum_max += col[cell->col + k].maxw;
                    if (sum_max < mx)
                        col[cell->col + n - 1].maxw += mx - sum_max;
                }
                for (k = 0; k < n; k++)
                    if (col[cell->col + k].maxw < col[cell->col + k].minw)
                        col[cell->col + k].maxw = col[cell->col + k].minw;
            }
    }
    return col;
}

static void table_intrinsic(struct lay_ctx *c, struct lay_box *t,
                            int32_t *minw, int32_t *maxw, int depth)
{
    int nrows = 0;
    int ncols = table_build_grid(c, t, &nrows);
    struct tcol *col = table_measure(c, t, ncols, depth);
    int32_t sp = table_spacing(t), smin = 0, smax = 0, px = 0;
    int i;

    *minw = *maxw = 0;
    if (!col)
        return;
    for (i = 0; i < ncols; i++) {
        smin += col[i].minw;
        smax += col[i].maxw;
    }
    smin += sp * (ncols + 1);
    smax += sp * (ncols + 1);
    if (len_is_fixed_px(t->style->width, &px)) {
        int32_t cw = content_dim(px, t->style, hedge_bp(t->style));
        if (cw > smin) smin = cw;
        if (cw > smax) smax = cw;
    }
    *minw = smin;
    *maxw = smax;
}

/* Is this a fixed-layout table? An explicit `table-layout: fixed` wins;
 * the opt callback can still override for callers with their own policy.
 * The fallback recognises the shape a fixed table has - an explicit
 * table width and an explicit width on every cell of the first row -
 * which is exactly the case where the two algorithms are required to
 * agree anyway. */
static int table_is_fixed(struct lay_ctx *c, struct lay_box *t)
{
    struct lay_box *g, *r, *cell;
    int n = 0, spec = 0;

    if (c->L->opt.table_layout && t->node)
        return c->L->opt.table_layout(c->L->opt.table_ctx, t->node) != 0;
    if (t->style->table_layout == CSS_TABLELAYOUT_FIXED)
        return 1;
    if (len_is_auto(t->style->width))
        return 0;
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next) {
            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            for (cell = r->first_child; cell; cell = cell->next) {
                if (cell->kind != LAY_BOX_TABLE_CELL)
                    continue;
                n++;
                if (!len_is_auto(cell->style->width))
                    spec++;
            }
            return n > 0 && spec == n;
        }
    }
    return 0;
}

static void table_distribute(struct tcol *col, int ncols, int32_t d, int fixed)
{
    int32_t smin = 0, smax = 0, used = 0;
    int i, nfree = 0;

    if (ncols <= 0)
        return;
    if (fixed) {
        int32_t left = d;

        for (i = 0; i < ncols; i++) {
            if (col[i].spec >= 0) {
                col[i].w = col[i].spec;
                left -= col[i].w;
            } else {
                nfree++;
            }
        }
        if (left < 0) left = 0;
        for (i = 0; i < ncols; i++)
            if (col[i].spec < 0)
                col[i].w = nfree ? left / nfree : 0;
        used = 0;
        for (i = 0; i < ncols; i++)
            used += col[i].w;
        if (used < d)
            col[ncols - 1].w += d - used;
        return;
    }
    for (i = 0; i < ncols; i++) {
        smin += col[i].minw;
        smax += col[i].maxw;
    }
    if (d >= smax) {
        int32_t extra = d - smax;

        for (i = 0; i < ncols; i++)
            col[i].w = col[i].maxw + (smax > 0
                ? clamp32((long long)extra * col[i].maxw / smax)
                : extra / ncols);
    } else if (d > smin && smax > smin) {
        int32_t room = d - smin, span = smax - smin;

        for (i = 0; i < ncols; i++)
            col[i].w = col[i].minw +
                clamp32((long long)room * (col[i].maxw - col[i].minw) / span);
    } else {
        for (i = 0; i < ncols; i++)
            col[i].w = col[i].minw;
    }
    for (i = 0; i < ncols; i++)
        used += col[i].w;
    if (used < d && d >= smin)
        col[ncols - 1].w += d - used;
}

static void layout_table(struct lay_ctx *c, struct lay_box *t, int32_t cb_x,
                         int32_t cb_w, int32_t y, int mode, int depth)
{
    const struct computed_style *cs = t->style;
    struct lay_box *g, *r, *cell, *cap;
    struct tcol *col;
    int32_t *colx, *rowh, *rowy;
    int32_t sp, content_w, d, cy, total_h = 0, cap_h = 0, h_bp, v_bp;
    int ncols, nrows = 0, i, fixed;
    char here;

    if ((uintptr_t)&here < c->stack_lo)
        c->stack_lo = (uintptr_t)&here;
    if ((uintptr_t)&here > c->stack_hi)
        c->stack_hi = (uintptr_t)&here;
    if (depth >= LAY_MAX_DEPTH) {
        c->L->trunc |= LAY_TRUNC_DEPTH;
        return;
    }

    resolve_edges(t, cb_w);
    h_bp = box_h_bp(t);
    v_bp = box_v_bp(t);
    t->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
        ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
    t->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
        ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);

    ncols = table_build_grid(c, t, &nrows);
    col = table_measure(c, t, ncols, depth);
    sp = table_spacing(t);
    fixed = table_is_fixed(c, t);

    if (!col || ncols <= 0) {
        if (mode != LAY_MODE_FIXEDW)
            t->w = len_is_auto_or_indef(cs->width, cb_w) ? 0
                : content_dim(used_len(cs->width, cb_w, 0), cs, h_bp);
        t->w = clamp_width(t->w, cs, cb_w, h_bp);
        t->h = 0;
        t->x = cb_x + t->marg[CSS_LEFT] + t->bord[CSS_LEFT] + t->pad[CSS_LEFT];
        t->y = y + t->bord[CSS_TOP] + t->pad[CSS_TOP];
        return;
    }

    {
        int32_t smin = 0, smax = 0, edges, avail;

        for (i = 0; i < ncols; i++) {
            smin += col[i].minw;
            smax += col[i].maxw;
        }
        smin += sp * (ncols + 1);
        smax += sp * (ncols + 1);
        edges = h_bp + t->marg[CSS_LEFT] + t->marg[CSS_RIGHT];
        avail = cb_w - edges;
        if (mode == LAY_MODE_SHRINK && avail > smax)
            avail = smax;
        if (mode == LAY_MODE_FIXEDW) {
            content_w = t->w;
        } else if (!len_is_auto_or_indef(cs->width, cb_w)) {
            content_w = content_dim(used_len(cs->width, cb_w, 0), cs, h_bp);
            if (content_w < smin)
                content_w = smin;
        } else {
            content_w = smax;
            if (content_w > avail)
                content_w = imax(avail, smin);
        }
        content_w = clamp_width(content_w, cs, cb_w, h_bp);
        t->w = content_w;
        if (mode != LAY_MODE_FIXEDW &&
            len_is_auto(cs->margin[CSS_LEFT]) &&
            len_is_auto(cs->margin[CSS_RIGHT])) {
            int32_t rest = cb_w - content_w - h_bp;

            if (rest > 0) {
                t->marg[CSS_LEFT] = rest / 2;
                t->marg[CSS_RIGHT] = rest - rest / 2;
            }
        } else if (mode != LAY_MODE_FIXEDW &&
                   len_is_auto(cs->margin[CSS_LEFT])) {
            t->marg[CSS_LEFT] = imax(0, cb_w - content_w - h_bp -
                                     t->marg[CSS_RIGHT]);
        }
    }
    t->x = cb_x + t->marg[CSS_LEFT] + t->bord[CSS_LEFT] + t->pad[CSS_LEFT];
    t->y = y + t->bord[CSS_TOP] + t->pad[CSS_TOP];

    d = t->w - sp * (ncols + 1);
    if (d < 0) d = 0;
    table_distribute(col, ncols, d, fixed);

    colx = (int32_t *)lay_arena_alloc(c->L, sizeof(int32_t) * (ncols + 1));
    rowh = (int32_t *)lay_arena_alloc(c->L, sizeof(int32_t) *
                                      (nrows > 0 ? nrows : 1));
    rowy = (int32_t *)lay_arena_alloc(c->L, sizeof(int32_t) *
                                      (nrows > 0 ? nrows : 1));
    if (!colx || !rowh || !rowy) {
        t->h = 0;
        return;
    }
    colx[0] = t->x + sp;
    for (i = 0; i < ncols; i++)
        colx[i + 1] = colx[i] + col[i].w + sp;
    for (i = 0; i < nrows; i++)
        rowh[i] = rowy[i] = 0;

    /* Captions sit above the rows and span the table. */
    cy = t->y;
    for (cap = t->first_child; cap; cap = cap->next) {
        struct lay_marg open = marg_of(0);
        struct lay_bfc cbfc;

        if (cap->kind != LAY_BOX_BLOCK ||
            cap->style->display != CSS_DISPLAY_TABLE_CAPTION)
            continue;
        cbfc.base = c->nfl;
        cbfc.cx = t->x;
        cbfc.cw = t->w;
        cbfc.low_left = cbfc.low_right = 0;
        cy = layout_block_box(c, cap, t->x, t->w, -1, cy, &cbfc, &open, 0,
                              LAY_MODE_FLOW, depth + 1);
        c->nfl = cbfc.base;
    }
    cap_h = cy - t->y;

    /* Pass 1: lay every cell out at a provisional y of 0. */
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next) {
            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            for (cell = r->first_child; cell; cell = cell->next) {
                struct lay_marg open = marg_of(0);
                struct lay_bfc cbfc;
                int32_t cw = 0;
                int k, n;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->col >= ncols)
                    continue;
                n = cell->colspan;
                if (cell->col + n > ncols)
                    n = ncols - cell->col;
                for (k = 0; k < n; k++)
                    cw += col[cell->col + k].w;
                cw += sp * (n - 1);
                resolve_edges(cell, t->w);
                cell->marg[CSS_LEFT] = cell->marg[CSS_RIGHT] = 0;
                cell->marg[CSS_TOP] = cell->marg[CSS_BOTTOM] = 0;
                cell->w = imax(0, cw - box_h_bp(cell));
                cbfc.base = c->nfl;
                cbfc.cx = colx[cell->col];
                cbfc.cw = cw;
                cbfc.low_left = cbfc.low_right = 0;
                layout_block_box(c, cell, colx[cell->col], cw, -1, 0,
                                 &cbfc, &open, 1, LAY_MODE_FIXEDW, depth + 1);
                c->nfl = cbfc.base;
            }
        }
    }

    /* Pass 2: row heights, single-span cells first. */
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next)
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t hh;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->rowspan != 1)
                    continue;
                if (cell->row >= nrows)
                    continue;
                hh = lay_border_rect(cell).h;
                if (hh > rowh[cell->row])
                    rowh[cell->row] = hh;
            }
    }
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next)
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t have = 0, want;
                int k, n;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->rowspan <= 1)
                    continue;
                if (cell->row >= nrows)
                    continue;
                n = cell->rowspan;
                if (cell->row + n > nrows)
                    n = nrows - cell->row;
                for (k = 0; k < n; k++)
                    have += rowh[cell->row + k];
                have += sp * (n - 1);
                want = lay_border_rect(cell).h;
                if (want > have && n > 0)
                    rowh[cell->row + n - 1] += want - have;
            }
    }
    {
        int32_t ry = t->y + cap_h + sp;

        for (i = 0; i < nrows; i++) {
            rowy[i] = ry;
            ry += rowh[i] + sp;
        }
        total_h = (nrows ? ry - (t->y + cap_h) : sp) ;
        if (!nrows)
            total_h = 0;
    }

    /* Pass 3: move each cell to its row and stretch it. */
    for (g = t->first_child; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next) {
            int32_t rtop = 0, rbot = 0;
            int have_row = 0;

            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            for (cell = r->first_child; cell; cell = cell->next) {
                int32_t span_h = 0, inner, nat, dy;
                int k, n;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->row >= nrows)
                    continue;
                n = cell->rowspan < 1 ? 1 : cell->rowspan;
                if (cell->row + n > nrows)
                    n = nrows - cell->row;
                for (k = 0; k < n; k++)
                    span_h += rowh[cell->row + k];
                span_h += sp * (n - 1);

                nat = cell->h;
                inner = span_h - cell->bord[CSS_TOP] - cell->pad[CSS_TOP] -
                        cell->bord[CSS_BOTTOM] - cell->pad[CSS_BOTTOM];
                if (inner < 0)
                    inner = 0;
                /* Move the whole cell from y = 0 to its row. */
                dy = rowy[cell->row] + cell->bord[CSS_TOP] +
                     cell->pad[CSS_TOP] - cell->y;
                translate_subtree(cell, 0, dy);
                cell->h = inner;

                /* vertical-align inside the cell */
                if (inner > nat) {
                    int32_t shift = 0;

                    switch (cell->style->vertical_align) {
                    case CSS_VALIGN_MIDDLE:
                        shift = (inner - nat) / 2;
                        break;
                    case CSS_VALIGN_BOTTOM:
                    case CSS_VALIGN_TEXT_BOTTOM:
                        shift = inner - nat;
                        break;
                    default:
                        shift = 0;
                        break;
                    }
                    if (shift) {
                        struct lay_box *k2;

                        for (k2 = cell->first_child; k2; k2 = k2->next)
                            translate_subtree(k2, 0, shift);
                    }
                }
                if (!have_row) {
                    rtop = lay_border_rect(cell).y;
                    rbot = rtop + span_h;
                    have_row = 1;
                }
            }
            if (have_row && r->row < nrows) {
                r->x = t->x + sp;
                r->y = rowy[r->row];
                r->w = imax(0, t->w - 2 * sp);
                r->h = rowh[r->row];
            }
            (void)rtop;
            (void)rbot;
        }
        /* the row group spans its rows */
        {
            struct lay_box *fr = 0, *lr = 0;

            for (r = g->first_child; r; r = r->next)
                if (r->kind == LAY_BOX_TABLE_ROW) {
                    if (!fr) fr = r;
                    lr = r;
                }
            if (fr && lr) {
                g->x = fr->x;
                g->y = fr->y;
                g->w = fr->w;
                g->h = lr->y + lr->h - fr->y;
            }
        }
    }

    t->h = cap_h + total_h;
    if (!len_is_auto_or_indef(cs->height, -1)) {
        int32_t want = content_dim(used_len(cs->height, -1, t->h), cs, v_bp);

        if (want > t->h)
            t->h = want;
    }
    t->h = clamp_height(t->h, cs, -1, v_bp);
    c->L->stats.tables += 0;
}

/* ================================================================== *
 * 10  out of flow: absolute, fixed, relative
 * ================================================================== */

static struct lay_box *abs_containing_block(struct lay_ctx *c,
                                            struct lay_box *b)
{
    struct lay_box *p;
    int guard = 0;

    for (p = b->parent; p && guard++ < LAY_MAX_DEPTH * 2; p = p->parent) {
        if (p->style && p->style->has_transform && !(p->flags & LAYF_ANON) &&
            p->kind != LAY_BOX_INLINE)
            return p;
        if (!(b->flags & LAYF_FIXED) && (p->flags & LAYF_POSITIONED))
            return p;
    }
    return c->L->icb;
}

static void layout_absolute(struct lay_ctx *c, struct lay_box *b, int depth)
{
    struct lay_box *cb = abs_containing_block(c, b);
    const struct computed_style *cs = b->style;
    lay_rect pb;
    int32_t sx = b->x, sy = b->y;     /* the static position, stashed  */
    int32_t cbx, cby, cbw, cbh, w, h, left, top, h_bp, v_bp;
    int auto_l, auto_r, auto_t, auto_b, auto_w, auto_h;
    struct lay_marg open = marg_of(0);
    struct lay_bfc bfc;

    if (cb == c->L->icb) {
        pb = lay_content_rect(cb);
        if (b->flags & LAYF_FIXED) {
            pb.w = c->L->opt.viewport_w;
            pb.h = c->L->opt.viewport_h;
        }
    } else {
        b->flags &= ~LAYF_FIXED;
        pb = lay_padding_rect(cb);
    }
    cbx = pb.x;
    cby = pb.y;
    cbw = pb.w;
    cbh = pb.h;

    resolve_edges(b, cbw);
    h_bp = box_h_bp(b);
    v_bp = box_v_bp(b);
    b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
        ? 0 : used_len(cs->margin[CSS_LEFT], cbw, 0);
    b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
        ? 0 : used_len(cs->margin[CSS_RIGHT], cbw, 0);

    auto_l = len_is_auto(cs->offset[CSS_LEFT]);
    auto_r = len_is_auto(cs->offset[CSS_RIGHT]);
    auto_t = len_is_auto(cs->offset[CSS_TOP]);
    auto_b = len_is_auto(cs->offset[CSS_BOTTOM]);
    auto_w = len_is_auto_or_indef(cs->width, cbw);
    auto_h = len_is_auto_or_indef(cs->height, cbh);

    {
        int32_t hmbp = b->marg[CSS_LEFT] + h_bp + b->marg[CSS_RIGHT];
        int32_t l = auto_l ? 0 : used_len(cs->offset[CSS_LEFT], cbw, 0);
        int32_t r = auto_r ? 0 : used_len(cs->offset[CSS_RIGHT], cbw, 0);

        if (!auto_w)
            w = content_dim(used_len(cs->width, cbw, 0), cs, h_bp);
        else if (!auto_l && !auto_r)
            w = imax(0, cbw - l - r - hmbp);
        else if (!auto_h && cs->aspect_ratio > 0) {
            int32_t dh = clamp_height(
                content_dim(used_len(cs->height, cbh, 0), cs, v_bp),
                cs, cbh, v_bp);
            w = ratio_width_from_height(cs, dh, h_bp, v_bp);
        } else {
            int32_t mn = 0, mx = 0, avail;

            intrinsic_widths(c, b, &mn, &mx, depth);
            mn -= hmbp;
            mx -= hmbp;
            if (mn < 0) mn = 0;
            if (mx < mn) mx = mn;
            avail = cbw - hmbp - (auto_l ? 0 : l) - (auto_r ? 0 : r);
            w = imin(imax(mn, avail), mx);
        }
        w = clamp_width(w, cs, cbw, h_bp);

        if (auto_l && auto_r)
            left = sx - cbx;                 /* keep the static position */
        else if (auto_l)
            left = cbw - r - w - hmbp;
        else
            left = l;
        b->w = w;
        b->x = cbx + left + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] +
               b->pad[CSS_LEFT];
    }

    /* Lay the content out at a provisional origin, then move it once the
     * vertical offsets are known. */
    bfc.base = c->nfl;
    bfc.cx = b->x;
    bfc.cw = b->w;
    bfc.low_left = bfc.low_right = 0;
    {
        int32_t save_x = b->x;

        layout_block_box(c, b, b->x - b->marg[CSS_LEFT] - b->bord[CSS_LEFT] -
                         b->pad[CSS_LEFT], cbw, cbh, 0, &bfc, &open, 1,
                         LAY_MODE_FIXEDW, depth);
        c->nfl = bfc.base;
        b->x = save_x;
    }

    h = b->h;
    if (!auto_h)
        h = content_dim(used_len(cs->height, cbh, h), cs, v_bp);
    else if (!auto_t && !auto_b) {
        int32_t t = used_len(cs->offset[CSS_TOP], cbh, 0);
        int32_t bo = used_len(cs->offset[CSS_BOTTOM], cbh, 0);
        int32_t vmbp = b->marg[CSS_TOP] + v_bp + b->marg[CSS_BOTTOM];

        h = imax(0, cbh - t - bo - vmbp);
    }
    h = clamp_height(h, cs, cbh, v_bp);
    b->h = h;

    {
        int32_t vmbp = b->marg[CSS_TOP] + b->bord[CSS_TOP] + b->pad[CSS_TOP] +
                       b->pad[CSS_BOTTOM] + b->bord[CSS_BOTTOM] +
                       b->marg[CSS_BOTTOM];
        int32_t t = auto_t ? 0 : used_len(cs->offset[CSS_TOP], cbh, 0);
        int32_t bo = auto_b ? 0 : used_len(cs->offset[CSS_BOTTOM], cbh, 0);

        if (auto_t && auto_b)
            top = sy - cby;
        else if (auto_t)
            top = cbh - bo - h - vmbp;
        else
            top = t;
        {
            int32_t want_y = cby + top + b->marg[CSS_TOP] + b->bord[CSS_TOP] +
                             b->pad[CSS_TOP];

            translate_subtree(b, 0, want_y - b->y);
        }
    }
}

static void apply_relative(struct lay_document *L)
{
    struct lay_box *n = L->icb;

    for (;;) {
        const struct computed_style *cs = n->style;

        if (cs && cs->position == CSS_POSITION_RELATIVE) {
            int32_t cbw = n->parent ? n->parent->w : L->opt.viewport_w;
            int32_t cbh = n->parent ? n->parent->h : L->opt.viewport_h;
            int32_t dx = 0, dy = 0;

            if (!len_is_auto(cs->offset[CSS_LEFT]))
                dx = used_len(cs->offset[CSS_LEFT], cbw, 0);
            else if (!len_is_auto(cs->offset[CSS_RIGHT]))
                dx = -used_len(cs->offset[CSS_RIGHT], cbw, 0);
            if (!len_is_auto(cs->offset[CSS_TOP]))
                dy = used_len(cs->offset[CSS_TOP], cbh, 0);
            else if (!len_is_auto(cs->offset[CSS_BOTTOM]))
                dy = -used_len(cs->offset[CSS_BOTTOM], cbh, 0);
            translate_relative_subtree(n, dx, dy);
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != L->icb && !n->next)
            n = n->parent;
        if (n == L->icb)
            break;
        n = n->next;
    }
}

static int32_t xform_scale_coord(int32_t v, int32_t orig, int32_t sc, int32_t t)
{
    int64_t m = (int64_t)(v - orig) * sc;
    int64_t q = m >= 0 ? (m + 500) / 1000 : -((-m + 500) / 1000);
    return clamp32((int64_t)orig + q + t);
}

static int32_t xform_scale_dim(int32_t v, int32_t sc)
{
    int64_t a = sc < 0 ? -(int64_t)sc : (int64_t)sc;
    if (v <= 0)
        return 0;
    return clamp32(((int64_t)v * a + 500) / 1000);
}

static void transform_scale_subtree(struct lay_box *b, int32_t ox, int32_t oy,
                                    int32_t sx, int32_t sy,
                                    int32_t tx, int32_t ty)
{
    struct lay_box *n = b;

    for (;;) {
        int fixed_boundary = n != b && (n->flags & LAYF_FIXED);

        if (!fixed_boundary) {
            int32_t x0 = xform_scale_coord(n->x, ox, sx, tx);
            int32_t x1 = xform_scale_coord(n->x + n->w, ox, sx, tx);
            int32_t y0 = xform_scale_coord(n->y, oy, sy, ty);
            int32_t y1 = xform_scale_coord(n->y + n->h, oy, sy, ty);

            if (x0 <= x1) { n->x = x0; n->w = x1 - x0; }
            else          { n->x = x1; n->w = x0 - x1; }
            if (y0 <= y1) { n->y = y0; n->h = y1 - y0; }
            else          { n->y = y1; n->h = y0 - y1; }

            n->pad[CSS_LEFT]    = xform_scale_dim(n->pad[CSS_LEFT], sx);
            n->pad[CSS_RIGHT]   = xform_scale_dim(n->pad[CSS_RIGHT], sx);
            n->pad[CSS_TOP]     = xform_scale_dim(n->pad[CSS_TOP], sy);
            n->pad[CSS_BOTTOM]  = xform_scale_dim(n->pad[CSS_BOTTOM], sy);
            n->bord[CSS_LEFT]   = xform_scale_dim(n->bord[CSS_LEFT], sx);
            n->bord[CSS_RIGHT]  = xform_scale_dim(n->bord[CSS_RIGHT], sx);
            n->bord[CSS_TOP]    = xform_scale_dim(n->bord[CSS_TOP], sy);
            n->bord[CSS_BOTTOM] = xform_scale_dim(n->bord[CSS_BOTTOM], sy);
            if (sx < 0) {
                int32_t t = n->pad[CSS_LEFT];
                n->pad[CSS_LEFT] = n->pad[CSS_RIGHT];
                n->pad[CSS_RIGHT] = t;
                t = n->bord[CSS_LEFT];
                n->bord[CSS_LEFT] = n->bord[CSS_RIGHT];
                n->bord[CSS_RIGHT] = t;
            }
            if (sy < 0) {
                int32_t t = n->pad[CSS_TOP];
                n->pad[CSS_TOP] = n->pad[CSS_BOTTOM];
                n->pad[CSS_BOTTOM] = t;
                t = n->bord[CSS_TOP];
                n->bord[CSS_TOP] = n->bord[CSS_BOTTOM];
                n->bord[CSS_BOTTOM] = t;
            }
            n->baseline = xform_scale_dim(n->baseline, sy);
        }
        if (!fixed_boundary && n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != b && !n->next)
            n = n->parent;
        if (n == b)
            break;
        n = n->next;
    }
}

static void apply_box_transform(struct lay_box *b)
{
    const struct computed_style *cs = b->style;
    lay_rect br;
    int32_t tx, ty, sx, sy;

    if ((b->flags & LAYF_ANON) || b->kind == LAY_BOX_INLINE ||
        b->kind == LAY_BOX_LINE || b->kind == LAY_BOX_TEXT ||
        b->kind == LAY_BOX_MARKER || !cs || !cs->has_transform)
        return;

    br = lay_border_rect(b);
    tx = used_len(cs->translate_x, br.w, 0);
    ty = used_len(cs->translate_y, br.h, 0);
    sx = cs->scale_x;
    sy = cs->scale_y;
    if (sx == 1000 && sy == 1000) {
        translate_relative_subtree(b, tx, ty);
        return;
    }
    transform_scale_subtree(b, br.x + br.w / 2, br.y + br.h / 2,
                            sx, sy, tx, ty);
}

static void apply_transforms(struct lay_document *L)
{
    struct lay_box *n = L->icb;
    int descending = 1;

    for (;;) {
        if (descending) {
            if (n->first_child) {
                n = n->first_child;
                continue;
            }
        }
        for (;;) {
            if (n != L->icb)
                apply_box_transform(n);
            if (n == L->icb)
                return;
            if (n->next) {
                n = n->next;
                descending = 1;
                break;
            }
            n = n->parent;
            descending = 0;
        }
    }
}

/* ================================================================== *
 * 11  post pass: clips, ink extents, paint order, node index
 * ================================================================== */

#define LAY_BIG 0x20000000

static void mark_positioning_contexts(struct lay_document *L)
{
    struct lay_box *n = L->icb;

    for (;;) {
        if (n != L->icb && n->parent) {
            uint32_t pf = n->parent->flags;

            if (!(n->flags & LAYF_FIXED)) {
                if (pf & (LAYF_FIXED | LAYF_FIXED_CTX))
                    n->flags |= LAYF_FIXED_CTX;
                else if (pf & (LAYF_STICKY | LAYF_STICKY_CTX))
                    n->flags |= LAYF_STICKY_CTX;
            }
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != L->icb && !n->next)
            n = n->parent;
        if (n == L->icb)
            break;
        n = n->next;
    }
}

static void compute_clips(struct lay_document *L)
{
    struct lay_box *n = L->icb;

    n->clip_x = -LAY_BIG;
    n->clip_y = -LAY_BIG;
    n->clip_w = 2 * LAY_BIG;
    n->clip_h = 2 * LAY_BIG;
    for (;;) {
        if (n != L->icb) {
            lay_rect c = ((n->flags & LAYF_FIXED) || !n->parent)
                ? lay_mkrect(-LAY_BIG, -LAY_BIG, 2 * LAY_BIG, 2 * LAY_BIG)
                : lay_mkrect(n->parent->clip_x, n->parent->clip_y,
                             n->parent->clip_w, n->parent->clip_h);

            if (n->flags & LAYF_CLIP)
                c = lay_rect_intersect(c, lay_padding_rect(n));
            n->clip_x = c.x;
            n->clip_y = c.y;
            n->clip_w = c.w;
            n->clip_h = c.h;
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != L->icb && !n->next)
            n = n->parent;
        if (n == L->icb)
            break;
        n = n->next;
    }
}

/* Post-order, iterative: a box's ink extent is its own border+shadow box
 * united with every non-fixed descendant's, which is what lets a
 * dirty-rectangle repaint reject an entire subtree with one comparison. */
static void compute_ink(struct lay_document *L)
{
    struct lay_box *n = L->icb;
    int descending = 1;

    for (;;) {
        if (descending) {
            lay_rect own = (n->kind == LAY_BOX_TEXT || n->kind == LAY_BOX_MARKER)
                ? lay_mkrect(n->x, n->y, n->w, n->h)
                : lay_paint_rect(n);

            n->ink_x = own.x;
            n->ink_y = own.y;
            n->ink_w = own.w;
            n->ink_h = own.h;
            if (n->first_child) {
                n = n->first_child;
                continue;
            }
        }
        for (;;) {
            struct lay_box *p = n->parent;

            if (p && !(n->flags & LAYF_FIXED)) {
                lay_rect u = lay_rect_union(lay_ink_rect(p), lay_ink_rect(n));
                lay_rect cl = lay_mkrect(p->clip_x, p->clip_y,
                                         p->clip_w, p->clip_h);

                if (p->flags & LAYF_CLIP)
                    u = lay_rect_intersect(u, lay_rect_union(
                            lay_paint_rect(p), cl));
                p->ink_x = u.x;
                p->ink_y = u.y;
                p->ink_w = u.w;
                p->ink_h = u.h;
            }
            if (n == L->icb)
                return;
            if (n->next) {
                n = n->next;
                descending = 1;
                break;
            }
            n = n->parent;
            if (!n)
                return;
            descending = 0;
        }
    }
}

static void compute_scroll(struct lay_document *L)
{
    struct lay_box *n = L->icb;

    for (;;) {
        n->scroll_w = n->w;
        n->scroll_h = n->h;
        if (n->flags & LAYF_SCROLL) {
            struct lay_box *ch;

            for (ch = n->first_child; ch; ch = ch->next) {
                lay_rect r = lay_ink_rect(ch);

                if (r.x + r.w - n->x > n->scroll_w)
                    n->scroll_w = r.x + r.w - n->x;
                if (r.y + r.h - n->y > n->scroll_h)
                    n->scroll_h = r.y + r.h - n->y;
            }
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != L->icb && !n->next)
            n = n->parent;
        if (n == L->icb)
            break;
        n = n->next;
    }
}

/* ---- node index -------------------------------------------------- */

static unsigned long ptr_hash(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;

    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 29;
    return (unsigned long)v;
}

static void build_node_index(struct lay_document *L)
{
    unsigned long cap = 64;
    struct lay_box *n;

    while (cap < L->nboxes * 2)
        cap <<= 1;
    L->hash = (struct lay_box **)malloc(cap * sizeof(struct lay_box *));
    if (!L->hash) {
        L->hmask = 0;
        return;
    }
    memset(L->hash, 0, cap * sizeof(struct lay_box *));
    L->hmask = cap - 1;

    n = L->icb;
    for (;;) {
        if (n->node && !(n->flags & LAYF_ANON)) {
            unsigned long i = ptr_hash(n->node) & L->hmask;
            unsigned long guard = 0;

            while (L->hash[i] && L->hash[i]->node != n->node &&
                   guard++ <= L->hmask)
                i = (i + 1) & L->hmask;
            if (!L->hash[i])
                L->hash[i] = n;
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n != L->icb && !n->next)
            n = n->parent;
        if (n == L->icb)
            break;
        n = n->next;
    }
}

/* ---- paint order ------------------------------------------------- */

struct pkey {
    int32_t rank;
    uint8_t bucket;
    uint8_t phase;
    uint32_t order;
    struct lay_box *box;
};

static int pkey_less(const struct pkey *a, const struct pkey *b)
{
    if (a->rank != b->rank) return a->rank < b->rank;
    if (a->bucket != b->bucket) return a->bucket < b->bucket;
    if (a->order != b->order) return a->order < b->order;
    return a->phase < b->phase;
}

static void pkey_sort(struct pkey *a, struct pkey *tmp, int n)
{
    int width, i;

    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            int l = i, m = i + width, r = i + 2 * width;
            int p = l, q = m, k = l;

            if (m > n) m = n;
            if (r > n) r = n;
            while (p < m && q < r)
                tmp[k++] = pkey_less(&a[q], &a[p]) ? a[q++] : a[p++];
            while (p < m) tmp[k++] = a[p++];
            while (q < r) tmp[k++] = a[q++];
        }
        for (i = 0; i < n; i++)
            a[i] = tmp[i];
    }
}

static int posn_less(const struct lay_box *a, const struct lay_box *b)
{
    if (a->z != b->z) return a->z < b->z;
    return a->order < b->order;
}

static void build_paint_order(struct lay_document *L)
{
    struct lay_box **pos = 0;
    struct pkey *keys = 0, *tmp = 0;
    struct lay_paint_item *paint = 0;
    struct lay_box *n;
    unsigned long np = 0, nk = 0, cap;
    unsigned long i;

    L->paint = 0;
    L->npaint = 0;

    /* Collect positioned boxes and rank them by (z-index, tree order). */
    pos = (struct lay_box **)malloc((L->nboxes + 1) * sizeof *pos);
    if (!pos)
        goto oom;
    n = L->icb;
    for (;;) {
        if ((n->flags & LAYF_POSITIONED) && n != L->icb)
            pos[np++] = n;
        if (n->first_child) { n = n->first_child; continue; }
        while (n != L->icb && !n->next) n = n->parent;
        if (n == L->icb) break;
        n = n->next;
    }
    /* insertion sort is fine: positioned boxes are a small minority, and
     * this keeps the code honest about ties */
    for (i = 1; i < np; i++) {
        struct lay_box *v = pos[i];
        unsigned long j = i;

        while (j > 0 && posn_less(v, pos[j - 1])) {
            pos[j] = pos[j - 1];
            j--;
        }
        pos[j] = v;
    }
    {
        unsigned long nneg = 0;

        for (i = 0; i < np; i++)
            if (pos[i]->z < 0)
                nneg++;
        for (i = 0; i < np; i++)
            pos[i]->z = (int32_t)(i < nneg ? (long)i - (long)nneg
                                           : (long)i - (long)nneg + 1);
    }

    cap = L->nboxes * 2 + 8;
    keys = (struct pkey *)malloc(cap * sizeof *keys);
    if (!keys)
        goto oom;
    tmp = (struct pkey *)malloc(cap * sizeof *tmp);
    if (!tmp)
        goto oom;

    n = L->icb;
    for (;;) {
        int32_t rank = 0;
        int bucket = 0, in_float = 0;
        struct lay_box *a;
        int guard = 0;

        for (a = n; a && guard++ < LAY_MAX_DEPTH * 2; a = a->parent) {
            if (a != L->icb && (a->flags & LAYF_POSITIONED)) { rank = a->z; break; }
            if (a->flags & LAYF_FLOAT) in_float = 1;
        }
        switch (n->kind) {
        case LAY_BOX_INLINE:
        case LAY_BOX_TEXT:
        case LAY_BOX_MARKER:
        case LAY_BOX_REPLACED:
            bucket = 2;
            break;
        default:
            bucket = 0;
            break;
        }
        if (in_float)
            bucket = 1;

        if (n != L->icb && nk + 2 < cap) {
            keys[nk].rank = rank;
            keys[nk].bucket = (uint8_t)bucket;
            keys[nk].phase = LAY_PHASE_BACKGROUND;
            keys[nk].order = n->order;
            keys[nk].box = n;
            nk++;
            if (n->kind == LAY_BOX_TEXT || n->kind == LAY_BOX_MARKER ||
                n->kind == LAY_BOX_REPLACED) {
                keys[nk].rank = rank;
                keys[nk].bucket = (uint8_t)bucket;
                keys[nk].phase = LAY_PHASE_CONTENT;
                keys[nk].order = n->order;
                keys[nk].box = n;
                nk++;
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != L->icb && !n->next) n = n->parent;
        if (n == L->icb) break;
        n = n->next;
    }
    pkey_sort(keys, tmp, (int)nk);

    paint = (struct lay_paint_item *)malloc(
        (nk + 1) * sizeof(struct lay_paint_item));
    if (!paint)
        goto oom;
    for (i = 0; i < nk; i++) {
        paint[i].box = keys[i].box;
        paint[i].phase = keys[i].phase;
    }
    L->paint = paint;
    L->npaint = (int)nk;
    free(pos);
    free(keys);
    free(tmp);
    return;

oom:
    L->trunc |= LAY_TRUNC_MEMORY;
    L->paint = 0;
    L->npaint = 0;
    free(pos);
    free(keys);
    free(tmp);
    free(paint);
}

/* ================================================================== *
 * 12  the public entry points
 * ================================================================== */

void lay_opts_init(struct lay_opts *o, int viewport_w, int viewport_h)
{
    memset(o, 0, sizeof *o);
    o->viewport_w = viewport_w > 0 ? viewport_w : 1024;
    o->viewport_h = viewport_h > 0 ? viewport_h : 768;
    o->default_font_size = 16;
    o->max_boxes = LAY_MAX_BOXES;
}

static void pick_canvas(struct lay_document *L, struct dom_document *doc)
{
    const struct computed_style *cs;

    L->canvas = 0xFFFFFFFFu;
    if (!doc)
        return;
    if (doc->body && doc->body->style) {
        cs = (const struct computed_style *)doc->body->style;
        if (CSS_COLOR_A(cs->background_color)) {
            L->canvas = cs->background_color;
            return;
        }
    }
    if (doc->html && doc->html->style) {
        cs = (const struct computed_style *)doc->html->style;
        if (CSS_COLOR_A(cs->background_color))
            L->canvas = cs->background_color;
    }
}

static struct lay_document *lay_run(struct dom_document *doc,
                                    struct dom_node *root,
                                    const struct lay_opts *o)
{
    struct lay_document *L;
    struct lay_ctx c;
    struct build_env env;
    struct lay_marg open;
    struct lay_bfc bfc;
    struct lay_box *rb;
    char here;
    int i;

    L = (struct lay_document *)malloc(sizeof *L);
    if (!L)
        return 0;
    memset(L, 0, sizeof *L);
    if (o)
        L->opt = *o;
    else
        lay_opts_init(&L->opt, 1024, 768);
    if (L->opt.viewport_w <= 0) L->opt.viewport_w = 1024;
    if (L->opt.viewport_h <= 0) L->opt.viewport_h = 768;
    if (L->opt.default_font_size <= 0) L->opt.default_font_size = 16;
    if (!L->opt.max_boxes || L->opt.max_boxes > LAY_MAX_BOXES)
        L->opt.max_boxes = LAY_MAX_BOXES;
    L->doc = doc;

    memset(&c, 0, sizeof c);
    c.L = L;
    c.stack_hi = c.stack_lo = (uintptr_t)&here;
    c.floats = (struct lay_float *)malloc(sizeof(struct lay_float) *
                                          LAY_MAX_FLOATS);
    c.frags = (struct lay_frag *)malloc(sizeof(struct lay_frag) *
                                        LAY_MAX_FRAGS);
    c.recs = (struct lay_rec *)malloc(sizeof(struct lay_rec) *
                                      (LAY_MAX_FRAGS / 4));
    c.posn = (struct lay_box **)malloc(sizeof(struct lay_box *) *
                                       LAY_MAX_POSITIONED);
    if (!c.floats || !c.frags || !c.recs || !c.posn) {
        free(c.floats); free(c.frags); free(c.recs); free(c.posn);
        free(L);
        return 0;
    }

    /* the initial containing block */
    L->icb = box_new(&c, LAY_BOX_BLOCK, lay_initial_style(L, 1), 0);
    if (!L->icb) {
        free(c.floats); free(c.frags); free(c.recs); free(c.posn);
        free(L);
        return 0;
    }
    L->icb->flags |= LAYF_ANON | LAYF_BFC;
    L->icb->w = L->opt.viewport_w;
    L->icb->h = L->opt.viewport_h;

    env.c = &c;
    env.in_link = 0;
    rb = root ? build_node(&env, root, style_of(L, root->parent), 0) : 0;
    if (rb) {
        box_append(L->icb, rb);
        L->root_el = rb;
        open = marg_of(0);
        bfc.base = 0;
        bfc.cx = 0;
        bfc.cw = L->opt.viewport_w;
        bfc.low_left = bfc.low_right = 0;
        c.nfl = 0;
        layout_block_box(&c, rb, 0, L->opt.viewport_w, L->opt.viewport_h,
                         0, &bfc, &open, 0, LAY_MODE_FLOW, 1);
    }

    /* Out-of-flow boxes, in the order they were discovered; the list can
     * grow while it is walked because an absolute box may contain more. */
    for (i = 0; i < c.nposn; i++) {
        c.nfl = 0;
        layout_absolute(&c, c.posn[i], 2);
    }

    apply_relative(L);
    apply_transforms(L);
    mark_positioning_contexts(L);

    if (rb) {
        lay_rect mr = lay_margin_rect(rb);

        L->icb->h = imax(L->opt.viewport_h, mr.y + mr.h);
        L->doc_w = imax(L->opt.viewport_w, mr.x + mr.w);
        L->doc_h = L->icb->h;
    } else {
        L->doc_w = L->opt.viewport_w;
        L->doc_h = L->opt.viewport_h;
    }

    compute_clips(L);
    compute_ink(L);
    compute_scroll(L);
    if (rb) {
        lay_rect ink = lay_ink_rect(rb);

        if (ink.y + ink.h > L->doc_h)
            L->doc_h = ink.y + ink.h;
        if (ink.x + ink.w > L->doc_w)
            L->doc_w = ink.x + ink.w;
    }
    build_node_index(L);
    build_paint_order(L);
    pick_canvas(L, doc);

    L->stats.boxes = L->nboxes;
    L->stats.max_depth = (unsigned long)c.depth_max;
    L->stats.stack_bytes = (unsigned long)(c.stack_hi - c.stack_lo);

    free(c.floats);
    free(c.frags);
    free(c.recs);
    free(c.posn);
    return L;
}

struct lay_document *lay_layout(struct dom_document *doc,
                                const struct lay_opts *o)
{
    struct dom_node *root;

    if (!doc)
        return 0;
    root = doc->html ? doc->html : doc->root;
    return lay_run(doc, root, o);
}

struct lay_document *lay_layout_node(struct dom_node *root,
                                     const struct lay_opts *o)
{
    if (!root)
        return 0;
    return lay_run(root->doc, root, o);
}

void lay_free(struct lay_document *L)
{
    struct lay_chunk *c, *n;

    if (!L)
        return;
    for (c = L->chunks; c; c = n) {
        n = c->next;
        free(c);
    }
    free(L->hash);
    free(L->paint);
    free(L);
}

struct lay_box *lay_root(const struct lay_document *L)
{
    return L ? L->icb : 0;
}

struct lay_box *lay_root_element(const struct lay_document *L)
{
    return L ? L->root_el : 0;
}

int32_t lay_width(const struct lay_document *L)  { return L ? L->doc_w : 0; }
int32_t lay_height(const struct lay_document *L) { return L ? L->doc_h : 0; }
unsigned long lay_box_count(const struct lay_document *L)
{
    return L ? L->nboxes : 0;
}
unsigned lay_truncated(const struct lay_document *L)
{
    return L ? L->trunc : 0;
}
uint32_t lay_canvas_color(const struct lay_document *L)
{
    return L ? L->canvas : 0xFFFFFFFFu;
}

unsigned long lay_memory_used(const struct lay_document *L)
{
    unsigned long n;

    if (!L)
        return 0;
    n = L->arena_bytes + sizeof *L;
    if (L->hash)
        n += (L->hmask + 1) * sizeof(struct lay_box *);
    if (L->paint)
        n += (unsigned long)L->npaint * sizeof(struct lay_paint_item);
    return n;
}

void lay_get_stats(const struct lay_document *L, struct lay_stats *out)
{
    if (!out)
        return;
    if (!L) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = L->stats;
    out->boxes = L->nboxes;
}

int lay_paint_order(const struct lay_document *L,
                    const struct lay_paint_item **out)
{
    if (!L || !L->paint) {
        if (out) *out = 0;
        return 0;
    }
    if (out)
        *out = L->paint;
    return L->npaint;
}

/* ---- hit testing & sticky offset ----------------------------------- */

static void sticky_box_offset(const struct lay_document *L,
                              const struct lay_box *s,
                              int32_t scroll_x, int32_t scroll_y,
                              int32_t *out_dx, int32_t *out_dy)
{
    const struct computed_style *cs = s->style;
    const struct lay_box *cb = s->parent ? s->parent : L->icb;
    lay_rect cbr, br;
    int32_t dx = 0, dy = 0;

    while (cb && (cb->flags & LAYF_ANON) && cb->parent)
        cb = cb->parent;
    cbr = lay_content_rect(cb);
    br = lay_border_rect(s);
    if (!cs) {
        *out_dx = 0;
        *out_dy = 0;
        return;
    }
    if (!len_is_auto(cs->offset[CSS_TOP])) {
        int32_t off = used_len(cs->offset[CSS_TOP], L->opt.viewport_h, 0);
        int32_t target = scroll_y + off;
        int32_t max_dy = (cbr.y + cbr.h) - (br.y + br.h);

        if (max_dy < 0)
            max_dy = 0;
        if (br.y < target) {
            dy = target - br.y;
            if (dy > max_dy)
                dy = max_dy;
        }
    }
    if (dy == 0 && !len_is_auto(cs->offset[CSS_BOTTOM])) {
        int32_t off = used_len(cs->offset[CSS_BOTTOM], L->opt.viewport_h, 0);
        int32_t target = scroll_y + L->opt.viewport_h - off;
        int32_t min_dy = cbr.y - br.y;

        if (min_dy > 0)
            min_dy = 0;
        if (br.y + br.h > target) {
            dy = target - (br.y + br.h);
            if (dy < min_dy)
                dy = min_dy;
        }
    }
    if (!len_is_auto(cs->offset[CSS_LEFT])) {
        int32_t off = used_len(cs->offset[CSS_LEFT], L->opt.viewport_w, 0);
        int32_t target = scroll_x + off;
        int32_t max_dx = (cbr.x + cbr.w) - (br.x + br.w);

        if (max_dx < 0)
            max_dx = 0;
        if (br.x < target) {
            dx = target - br.x;
            if (dx > max_dx)
                dx = max_dx;
        }
    }
    if (dx == 0 && !len_is_auto(cs->offset[CSS_RIGHT])) {
        int32_t off = used_len(cs->offset[CSS_RIGHT], L->opt.viewport_w, 0);
        int32_t target = scroll_x + L->opt.viewport_w - off;
        int32_t min_dx = cbr.x - br.x;

        if (min_dx > 0)
            min_dx = 0;
        if (br.x + br.w > target) {
            dx = target - (br.x + br.w);
            if (dx < min_dx)
                dx = min_dx;
        }
    }
    *out_dx = dx;
    *out_dy = dy;
}

void lay_sticky_offset(const struct lay_document *L, const struct lay_box *b,
                       int32_t scroll_x, int32_t scroll_y,
                       int32_t *dx, int32_t *dy)
{
    int32_t tx = 0, ty = 0;
    const struct lay_box *a = b;
    int guard = 0;

    if (!L || !b || (b->flags & (LAYF_FIXED | LAYF_FIXED_CTX)) ||
        !(b->flags & (LAYF_STICKY | LAYF_STICKY_CTX))) {
        if (dx) *dx = 0;
        if (dy) *dy = 0;
        return;
    }
    while (a && (a->flags & (LAYF_STICKY | LAYF_STICKY_CTX)) &&
           guard++ < LAY_MAX_DEPTH * 2) {
        if (a->flags & LAYF_FIXED)
            break;
        if (a->flags & LAYF_STICKY) {
            int32_t sx = 0, sy = 0;

            sticky_box_offset(L, a, scroll_x, scroll_y, &sx, &sy);
            tx += sx;
            ty += sy;
        }
        a = a->parent;
    }
    if (dx) *dx = tx;
    if (dy) *dy = ty;
}

struct lay_box *lay_hit_test_view(const struct lay_document *L,
                                  int32_t vx, int32_t vy,
                                  int32_t scroll_x, int32_t scroll_y)
{
    int i;

    if (!L || !L->paint)
        return 0;
    for (i = L->npaint - 1; i >= 0; i--) {
        struct lay_box *b = L->paint[i].box;
        int32_t bx, by;
        lay_rect cl, r;

        if (b->flags & LAYF_HIDDEN)
            continue;
        if (b->flags & (LAYF_FIXED | LAYF_FIXED_CTX)) {
            bx = 0;
            by = 0;
            cl = lay_clip_rect(b);
        } else if (b->flags & (LAYF_STICKY | LAYF_STICKY_CTX)) {
            int32_t sdx = 0, sdy = 0;
            const struct lay_box *s = b, *anc;
            lay_rect ac, oc;
            int guard = 0;

            lay_sticky_offset(L, b, scroll_x, scroll_y, &sdx, &sdy);
            bx = sdx - scroll_x;
            by = sdy - scroll_y;
            while (s && !(s->flags & LAYF_STICKY) &&
                   guard++ < LAY_MAX_DEPTH * 2)
                s = s->parent;
            if (s) {
                ac = s->parent ? lay_clip_rect(s->parent)
                               : lay_mkrect(-LAY_BIG, -LAY_BIG,
                                            2 * LAY_BIG, 2 * LAY_BIG);
                ac.x -= scroll_x;
                ac.y -= scroll_y;
                oc = lay_mkrect(-LAY_BIG, -LAY_BIG, 2 * LAY_BIG, 2 * LAY_BIG);
                for (anc = b; anc && guard++ < LAY_MAX_DEPTH * 4;
                     anc = anc->parent) {
                    if (anc->flags & LAYF_CLIP)
                        oc = lay_rect_intersect(oc, lay_padding_rect(anc));
                    if (anc == s)
                        break;
                }
                oc.x += bx;
                oc.y += by;
                cl = lay_rect_intersect(ac, oc);
            } else {
                cl = lay_clip_rect(b);
                cl.x += bx;
                cl.y += by;
            }
        } else {
            bx = -scroll_x;
            by = -scroll_y;
            cl = lay_clip_rect(b);
            cl.x += bx;
            cl.y += by;
        }
        if (!lay_rect_contains(cl, vx, vy))
            continue;
        r = (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_MARKER)
            ? lay_mkrect(b->x, b->y, b->w, b->h) : lay_border_rect(b);
        r.x += bx;
        r.y += by;
        if (r.w <= 0 || r.h <= 0)
            continue;
        if (lay_rect_contains(r, vx, vy))
            return b;
    }
    return 0;
}

struct lay_box *lay_hit_test(const struct lay_document *L, int32_t x, int32_t y)
{
    return lay_hit_test_view(L, x, y, 0, 0);
}

struct dom_node *lay_node_at(const struct lay_document *L, int32_t x, int32_t y)
{
    struct lay_box *b = lay_hit_test(L, x, y);
    int guard = 0;

    while (b && guard++ < LAY_MAX_DEPTH * 2) {
        if (b->node)
            return b->node;
        b = b->parent;
    }
    return 0;
}

struct dom_node *lay_link_at(const struct lay_document *L, int32_t x, int32_t y,
                             const char **href)
{
    struct dom_node *n = lay_node_at(L, x, y);
    int guard = 0;

    if (href)
        *href = 0;
    while (n && guard++ < DOM_MAX_DEPTH) {
        if (n->type == DOM_ELEMENT && n->tag_id == HTAG_A) {
            const char *h = dom_get_attr(n, "href");

            if (h) {
                if (href)
                    *href = h;
                return n;
            }
        }
        n = n->parent;
    }
    return 0;
}

struct lay_box *lay_box_for_node(const struct lay_document *L,
                                 const struct dom_node *n)
{
    unsigned long i, guard = 0;

    if (!L || !L->hash || !n)
        return 0;
    i = ptr_hash(n) & L->hmask;
    while (L->hash[i]) {
        if (L->hash[i]->node == n)
            return L->hash[i];
        if (guard++ > L->hmask)
            break;
        i = (i + 1) & L->hmask;
    }
    return 0;
}

int lay_boxes_for_node(const struct lay_document *L, const struct dom_node *n,
                       lay_box_fn cb, void *ctx)
{
    struct lay_box *b;
    int count = 0;

    if (!L || !n)
        return 0;
    b = L->icb;
    for (;;) {
        if (b->node == n) {
            count++;
            if (cb)
                cb(ctx, b);
        }
        if (b->first_child) { b = b->first_child; continue; }
        while (b != L->icb && !b->next) b = b->parent;
        if (b == L->icb) break;
        b = b->next;
    }
    return count;
}

int lay_scroll_to_id(const struct lay_document *L, const char *id, int32_t *y)
{
    struct dom_node *n;
    struct lay_box *b;

    if (!L || !L->doc || !id)
        return 0;
    n = dom_get_element_by_id(L->doc, id);
    if (!n)
        return 0;
    b = lay_box_for_node(L, n);
    if (!b)
        return 0;
    if (y)
        *y = lay_border_rect(b).y;
    return 1;
}
