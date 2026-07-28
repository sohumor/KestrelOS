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

/* min/max clamping. max-width: none is CSS_LEN_NONE. */
static int32_t clamp_width(int32_t w, const struct computed_style *cs,
                           int32_t base)
{
    int32_t mx, mn;

    if (cs->max_width.type != CSS_LEN_NONE &&
        cs->max_width.type != CSS_LEN_AUTO) {
        mx = used_len(cs->max_width, base, 0x3FFFFFFF);
        if (w > mx)
            w = mx;
    }
    mn = used_len(cs->min_width, base, 0);
    if (w < mn)
        w = mn;
    if (w < 0)
        w = 0;
    return w;
}

static int32_t clamp_height(int32_t h, const struct computed_style *cs,
                            int32_t base)
{
    int32_t mx, mn;

    if (cs->max_height.type != CSS_LEN_NONE &&
        cs->max_height.type != CSS_LEN_AUTO) {
        mx = used_len(cs->max_height, base, 0x3FFFFFFF);
        if (h > mx)
            h = mx;
    }
    mn = used_len(cs->min_height, base, 0);
    if (h < mn)
        h = mn;
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
        case CSS_DISPLAY_INLINE_GRID:
        case CSS_DISPLAY_GRID:
            return CSS_DISPLAY_BLOCK;
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

static void fixup_children(struct lay_ctx *c, struct lay_box *b)
{
    struct lay_box *ch, *nx, *anon = 0;
    struct lay_box *head = b->first_child, *tail = b->last_child;
    int has_block = 0;

    if (!head)
        return;
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
    int pre = (b->flags & LAYF_PRE) != 0;
    int wrap = ws_wraps(b->style->white_space);

    if (!pre) {
        while (i < b->text_len) {
            while (i < b->text_len && ws_is_space((unsigned char)b->text[i]))
                i++;
            start = i;
            while (i < b->text_len && !ws_is_space((unsigned char)b->text[i]))
                i++;
            if (i > start) {
                int32_t w = text_width(b->font, b->text + start, i - start);

                if (w > widest)
                    widest = w;
                if (total)
                    total += b->font->w;   /* the collapsed space */
                total += w;
            }
        }
        if (!wrap)
            widest = total;   /* nowrap: nothing may break */
        *minw = widest;
        *sumw = total;
        return;
    }
    for (i = 0; i <= b->text_len; i++) {
        if (i == b->text_len || b->text[i] == '\n') {
            if (line > widest) widest = line;
            if (line > total) total = line;
            line = 0;
        } else {
            line += b->font->w;
        }
    }
    if (wrap) {
        int32_t run = 0;

        widest = 0;
        for (i = 0; i < b->text_len; i++) {
            if (ws_is_space((unsigned char)b->text[i])) {
                run = 0;
            } else {
                run += b->font->w;
                if (run > widest)
                    widest = run;
            }
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

static void intrinsic_widths(struct lay_ctx *c, struct lay_box *b,
                             int32_t *minw, int32_t *maxw, int depth)
{
    const struct computed_style *cs = b->style;
    int32_t extra, inner_min = 0, inner_max = 0;
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
    extra = hedge_bp(cs) + hedge_margin(cs);

    if (b->kind == LAY_BOX_REPLACED) {
        int32_t w;

        if (cs->width.type == CSS_LEN_PX)
            w = cs->width.v;
        else
            w = b->intrinsic_w;
        *minw = *maxw = imax(0, w) + extra;
        return;
    }
    if (b->kind == LAY_BOX_TABLE) {
        table_intrinsic(c, b, &inner_min, &inner_max, depth);
        *minw = inner_min + extra;
        *maxw = inner_max + extra;
        return;
    }
    if (cs->width.type == CSS_LEN_PX) {
        inner_min = inner_max = imax(0, cs->width.v);
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
    if (cs->min_width.type == CSS_LEN_PX) {
        if (inner_min < cs->min_width.v) inner_min = cs->min_width.v;
        if (inner_max < cs->min_width.v) inner_max = cs->min_width.v;
    }
    if (cs->max_width.type == CSS_LEN_PX) {
        if (inner_min > cs->max_width.v) inner_min = cs->max_width.v;
        if (inner_max > cs->max_width.v) inner_max = cs->max_width.v;
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
    fr->w = f->sp_font->w;
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
    int32_t w = text_width(font, s, n);
    int32_t sp = f->pending_space ? f->sp_font->w : 0;
    int guard = 0;

    for (;;) {
        int32_t avail = ifc_avail(f);

        if (!wrap || f->pen + sp + w <= avail) {
            ifc_emit_space(f);
            ifc_emit(f, tb, s, n, w, 0);
            return;
        }
        if (ifc_has_content(f) && f->pen > 0) {
            f->pending_space = 0;
            ifc_break(f, 0);
            sp = 0;
            if (guard++ > 4)
                wrap = 0;      /* a zero-width line: stop retrying */
            continue;
        }
        /* The word is alone on the line and still too wide: break it at
         * the last character that fits, never fewer than one. */
        {
            unsigned long fit = font->w > 0
                ? (unsigned long)((avail - f->pen) / font->w) : 1;

            if (fit < 1) fit = 1;
            if (fit >= n) {
                ifc_emit_space(f);
                ifc_emit(f, tb, s, n, w, 0);
                return;
            }
            ifc_emit_space(f);
            ifc_emit(f, tb, s, fit, text_width(font, s, fit), 0);
            s += fit;
            n -= fit;
            w = text_width(font, s, n);
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
                int32_t w = text_width(tb->font, t + i, len);

                /* Preserved spaces are NOT trimmed at the end of a
                 * line - that is the whole point of `pre` - so they are
                 * marked 2 rather than 1. */
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
    sp = f->pending_space ? f->sp_font->w : 0;
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

    line_w = nf ? fg[nf - 1].x + fg[nf - 1].w : f->pen;
    if (!nf && !nr)
        line_w = 0;
    avail = ifc_avail(f);

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
    int auto_w = len_is_auto(cs->width) || cs->width.type == CSS_LEN_NONE;
    int auto_h = len_is_auto(cs->height) ||
                 (cs->height.type == CSS_LEN_PCT && cb_h < 0);

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
        } else if (!(cs->width.type == CSS_LEN_PX ||
                     cs->height.type == CSS_LEN_PX)) {
            iw = 0;
            ih = 0;
        }
    }

    w = auto_w ? -1 : used_len(cs->width, cb_w, -1);
    h = auto_h ? -1 : used_len(cs->height, cb_h, -1);

    if (w < 0 && h < 0) {
        w = iw;
        h = ih;
    } else if (w < 0) {
        w = (ih > 0) ? clamp32((long long)h * iw / ih) : iw;
    } else if (h < 0) {
        h = (iw > 0) ? clamp32((long long)w * ih / iw) : ih;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    {
        int32_t cw = clamp_width(w, cs, cb_w);
        int32_t chh;

        if (cw != w && auto_h && iw > 0)
            h = clamp32((long long)cw * ih / iw);
        w = cw;
        chh = clamp_height(h, cs, cb_h);
        if (chh != h && auto_w && ih > 0)
            w = clamp_width(clamp32((long long)chh * iw / ih), cs, cb_w);
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
                               int32_t cb_w, int mode, int depth)
{
    const struct computed_style *cs = b->style;
    int32_t spec, clamped;

    resolve_edges(b, cb_w);

    if (mode == LAY_MODE_FIXEDW) {
        b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
            ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
        b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
            ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);
        b->w = clamp_width(b->w, cs, cb_w);
        return;
    }
    if (mode == LAY_MODE_SHRINK) {
        int32_t edges = b->bord[CSS_LEFT] + b->pad[CSS_LEFT] +
                        b->bord[CSS_RIGHT] + b->pad[CSS_RIGHT];

        b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
            ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
        b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
            ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);
        if (len_is_auto(cs->width)) {
            int32_t mn = 0, mx = 0, avail;

            intrinsic_widths(c, b, &mn, &mx, depth);
            mn -= edges + b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];
            mx -= edges + b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];
            if (mn < 0) mn = 0;
            if (mx < mn) mx = mn;
            avail = cb_w - edges - b->marg[CSS_LEFT] - b->marg[CSS_RIGHT];
            b->w = imin(imax(mn, avail), mx);
        } else {
            b->w = used_len(cs->width, cb_w, 0);
        }
        b->w = clamp_width(b->w, cs, cb_w);
        return;
    }

    spec = len_is_auto(cs->width) ? -1 : used_len(cs->width, cb_w, -1);
    solve_width(b, cb_w, spec);
    clamped = clamp_width(b->w, cs, cb_w);
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
};

/* A bounded single-line flex formatting context.  It covers the common
 * navigation/toolbar/card-row case: row and row-reverse, flex-grow, gap,
 * justify-content and cross-axis start/end/center alignment.  Column flex
 * intentionally continues through normal block flow, which is already its
 * interoperable fallback. */
static int32_t layout_flex_row(struct lay_ctx *c, struct lay_box *b,
                               int32_t y, struct lay_bfc *bfc, int depth)
{
    struct flex_item *items;
    struct lay_box *ch;
    int count = 0, i, reverse;
    int64_t total = 0, grow_total = 0;
    int32_t gap = b->style->gap > 0 ? b->style->gap : 0;
    int32_t free_space, start = 0, gap_extra = 0, cursor, max_h = 0;

    for (ch = b->first_child; ch; ch = ch->next)
        if (ch->kind != LAY_BOX_MARKER && !(ch->flags & LAYF_ABSOLUTE))
            count++;
    if (!count)
        return 0;
    if (count > 512)
        return -1;
    items = (struct flex_item *)calloc((unsigned long)count, sizeof(*items));
    if (!items)
        return -1;
    i = 0;
    for (ch = b->first_child; ch; ch = ch->next) {
        int32_t basis, mn = 0, mx = 0;

        if (ch->kind == LAY_BOX_MARKER)
            continue;
        if (ch->flags & LAYF_ABSOLUTE) {
            record_out_of_flow(c, ch, b->x, y);
            continue;
        }
        if (ch->style->width.type == CSS_LEN_PX ||
            ch->style->width.type == CSS_LEN_PCT) {
            basis = used_len(ch->style->width, b->w, 0);
        } else if (ch->style->flex_grow > 0) {
            basis = 0;
        } else {
            intrinsic_widths(c, ch, &mn, &mx, depth + 1);
            basis = mx;
        }
        if (basis < 0) basis = 0;
        if (basis > b->w) basis = b->w;
        items[i].box = ch;
        items[i].basis = basis;
        items[i].allocated = basis;
        total += basis;
        grow_total += ch->style->flex_grow;
        i++;
    }
    total += (int64_t)gap * (count - 1);
    free_space = total < b->w ? b->w - (int32_t)total : 0;
    if (free_space > 0 && grow_total > 0) {
        int32_t remaining = free_space;
        int64_t grow_left = grow_total;

        for (i = 0; i < count; i++) {
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
        free_space = 0;
    } else if (total > b->w) {
        int64_t bases = total - (int64_t)gap * (count - 1);
        int32_t room = b->w - gap * (count - 1);

        if (room < count) room = count;
        if (bases > 0)
            for (i = 0; i < count; i++) {
                items[i].allocated = (int32_t)
                    ((int64_t)items[i].basis * room / bases);
                if (items[i].allocated < 1)
                    items[i].allocated = 1;
            }
        free_space = 0;
    }
    if (free_space > 0) {
        switch (b->style->justify_content) {
        case CSS_JUSTIFY_END:
            start = free_space;
            break;
        case CSS_JUSTIFY_CENTER:
            start = free_space / 2;
            break;
        case CSS_JUSTIFY_SPACE_BETWEEN:
            if (count > 1) gap_extra = free_space / (count - 1);
            break;
        case CSS_JUSTIFY_SPACE_AROUND:
            gap_extra = free_space / count;
            start = gap_extra / 2;
            break;
        case CSS_JUSTIFY_SPACE_EVENLY:
            gap_extra = free_space / (count + 1);
            start = gap_extra;
            break;
        default:
            break;
        }
    }
    reverse = b->style->flex_direction == CSS_FLEXDIR_ROW_REVERSE;
    cursor = b->x + start;
    for (i = 0; i < count; i++) {
        int at = reverse ? count - 1 - i : i;
        lay_rect r;

        ch = items[at].box;
        layout_standalone(c, ch, cursor, y, items[at].allocated,
                          0, depth + 1);
        r = lay_margin_rect(ch);
        if (r.h > max_h) max_h = r.h;
        cursor += items[at].allocated + gap + gap_extra;
    }
    if (b->style->align_items == CSS_ALIGN_END ||
        b->style->align_items == CSS_ALIGN_CENTER) {
        for (i = 0; i < count; i++) {
            lay_rect r = lay_margin_rect(items[i].box);
            int32_t dy = max_h - r.h;

            if (b->style->align_items == CSS_ALIGN_CENTER)
                dy /= 2;
            if (dy > 0)
                translate_subtree(items[i].box, 0, dy);
        }
    }
    free(items);
    (void)bfc;
    return max_h;
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

    resolve_horizontal(c, b, cb_w, mode, depth);
    auto_h = len_is_auto(cs->height) ||
             (cs->height.type == CSS_LEN_PCT && cb_h < 0);
    if (!auto_h) {
        definite_h = used_len(cs->height, cb_h, 0);
        definite_h = clamp_height(definite_h, cs, cb_h);
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
            int32_t edges = b->bord[CSS_LEFT] + b->pad[CSS_LEFT] +
                            b->bord[CSS_RIGHT] + b->pad[CSS_RIGHT] +
                            b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];

            if (len_is_auto(cs->width))
                b->w = clamp_width(imax(0, avail - edges), cs, cb_w);
            b->x = l + b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT];
        }
    }

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

    if (b->flags & LAYF_IFC) {
        int32_t mw = 0;

        h_children = layout_inline(c, b, &mine, content_y, &mw, depth);
        has_lines = h_children > 0;
        flow = content_y + h_children;
        open = marg_of(0);
    } else if ((cs->display == CSS_DISPLAY_FLEX ||
                cs->display == CSS_DISPLAY_INLINE_FLEX) &&
               (cs->flex_direction == CSS_FLEXDIR_ROW ||
                cs->flex_direction == CSS_FLEXDIR_ROW_REVERSE) &&
               (h_children = layout_flex_row(c, b, content_y,
                                             &mine, depth)) >= 0) {
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
            flow = layout_block_box(c, ch, b->x, b->w,
                                    auto_h ? -1 : definite_h,
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
    used_h = clamp_height(used_h, cs, cb_h);
    b->h = used_h;

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
                int32_t mn = 0, mx = 0;

                if (cell->kind != LAY_BOX_TABLE_CELL || cell->colspan != 1)
                    continue;
                if (cell->col >= ncols)
                    continue;
                intrinsic_widths(c, cell, &mn, &mx, depth + 1);
                if (mn > col[cell->col].minw) col[cell->col].minw = mn;
                if (mx > col[cell->col].maxw) col[cell->col].maxw = mx;
                if (cell->style->width.type == CSS_LEN_PX &&
                    cell->style->width.v > col[cell->col].spec)
                    col[cell->col].spec = cell->style->width.v +
                                          hedge_bp(cell->style);
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
    int32_t sp = table_spacing(t), smin = 0, smax = 0;
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
    if (t->style->width.type == CSS_LEN_PX) {
        if (t->style->width.v > smin) smin = t->style->width.v;
        if (t->style->width.v > smax) smax = t->style->width.v;
    }
    *minw = smin;
    *maxw = smax;
}

/* Is this a fixed-layout table? computed_style has no table-layout
 * property, so the decision is delegated; the fallback recognises the
 * shape a fixed table has - an explicit table width and an explicit
 * width on every cell of the first row - which is exactly the case where
 * the two algorithms are required to agree anyway. */
static int table_is_fixed(struct lay_ctx *c, struct lay_box *t)
{
    struct lay_box *g, *r, *cell;
    int n = 0, spec = 0;

    if (c->L->opt.table_layout && t->node)
        return c->L->opt.table_layout(c->L->opt.table_ctx, t->node) != 0;
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
    int32_t sp, content_w, d, cy, total_h = 0, cap_h = 0;
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
    t->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
        ? 0 : used_len(cs->margin[CSS_LEFT], cb_w, 0);
    t->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
        ? 0 : used_len(cs->margin[CSS_RIGHT], cb_w, 0);

    ncols = table_build_grid(c, t, &nrows);
    col = table_measure(c, t, ncols, depth);
    sp = table_spacing(t);
    fixed = table_is_fixed(c, t);

    if (!col || ncols <= 0) {
        t->w = len_is_auto(cs->width) ? 0 : used_len(cs->width, cb_w, 0);
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
        edges = t->bord[CSS_LEFT] + t->pad[CSS_LEFT] +
                t->bord[CSS_RIGHT] + t->pad[CSS_RIGHT] +
                t->marg[CSS_LEFT] + t->marg[CSS_RIGHT];
        avail = cb_w - edges;
        if (mode == LAY_MODE_SHRINK && avail > smax)
            avail = smax;
        if (!len_is_auto(cs->width)) {
            content_w = used_len(cs->width, cb_w, 0);
            if (content_w < smin)
                content_w = smin;
        } else {
            content_w = smax;
            if (content_w > avail)
                content_w = imax(avail, smin);
        }
        content_w = clamp_width(content_w, cs, cb_w);
        t->w = content_w;
        if (len_is_auto(cs->margin[CSS_LEFT]) &&
            len_is_auto(cs->margin[CSS_RIGHT])) {
            int32_t rest = cb_w - content_w - t->bord[CSS_LEFT] -
                           t->pad[CSS_LEFT] - t->bord[CSS_RIGHT] -
                           t->pad[CSS_RIGHT];

            if (rest > 0) {
                t->marg[CSS_LEFT] = rest / 2;
                t->marg[CSS_RIGHT] = rest - rest / 2;
            }
        } else if (len_is_auto(cs->margin[CSS_LEFT])) {
            t->marg[CSS_LEFT] = imax(0, cb_w - content_w - t->bord[CSS_LEFT] -
                                     t->pad[CSS_LEFT] - t->bord[CSS_RIGHT] -
                                     t->pad[CSS_RIGHT] - t->marg[CSS_RIGHT]);
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
                cell->w = imax(0, cw - cell->bord[CSS_LEFT] -
                               cell->pad[CSS_LEFT] - cell->bord[CSS_RIGHT] -
                               cell->pad[CSS_RIGHT]);
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
    if (!len_is_auto(cs->height)) {
        int32_t want = used_len(cs->height, -1, t->h);

        if (want > t->h)
            t->h = want;
    }
    t->h = clamp_height(t->h, cs, -1);
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

    if (b->flags & LAYF_FIXED)
        return c->L->icb;
    for (p = b->parent; p && guard++ < LAY_MAX_DEPTH * 2; p = p->parent)
        if (p->flags & LAYF_POSITIONED)
            return p;
    return c->L->icb;
}

static void layout_absolute(struct lay_ctx *c, struct lay_box *b, int depth)
{
    struct lay_box *cb = abs_containing_block(c, b);
    const struct computed_style *cs = b->style;
    lay_rect pb;
    int32_t sx = b->x, sy = b->y;     /* the static position, stashed  */
    int32_t cbx, cby, cbw, cbh, w, h, left, top;
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
        pb = lay_padding_rect(cb);
    }
    cbx = pb.x;
    cby = pb.y;
    cbw = pb.w;
    cbh = pb.h;

    resolve_edges(b, cbw);
    b->marg[CSS_LEFT] = len_is_auto(cs->margin[CSS_LEFT])
        ? 0 : used_len(cs->margin[CSS_LEFT], cbw, 0);
    b->marg[CSS_RIGHT] = len_is_auto(cs->margin[CSS_RIGHT])
        ? 0 : used_len(cs->margin[CSS_RIGHT], cbw, 0);

    auto_l = len_is_auto(cs->offset[CSS_LEFT]);
    auto_r = len_is_auto(cs->offset[CSS_RIGHT]);
    auto_t = len_is_auto(cs->offset[CSS_TOP]);
    auto_b = len_is_auto(cs->offset[CSS_BOTTOM]);
    auto_w = len_is_auto(cs->width);
    auto_h = len_is_auto(cs->height);

    {
        int32_t hmbp = b->marg[CSS_LEFT] + b->bord[CSS_LEFT] + b->pad[CSS_LEFT] +
                       b->pad[CSS_RIGHT] + b->bord[CSS_RIGHT] +
                       b->marg[CSS_RIGHT];
        int32_t l = auto_l ? 0 : used_len(cs->offset[CSS_LEFT], cbw, 0);
        int32_t r = auto_r ? 0 : used_len(cs->offset[CSS_RIGHT], cbw, 0);

        if (!auto_w)
            w = used_len(cs->width, cbw, 0);
        else if (!auto_l && !auto_r)
            w = imax(0, cbw - l - r - hmbp);
        else {
            int32_t mn = 0, mx = 0, avail;

            intrinsic_widths(c, b, &mn, &mx, depth);
            mn -= hmbp;
            mx -= hmbp;
            if (mn < 0) mn = 0;
            if (mx < mn) mx = mn;
            avail = cbw - hmbp - (auto_l ? 0 : l) - (auto_r ? 0 : r);
            w = imin(imax(mn, avail), mx);
        }
        w = clamp_width(w, cs, cbw);

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
        h = used_len(cs->height, cbh, h);
    else if (!auto_t && !auto_b) {
        int32_t t = used_len(cs->offset[CSS_TOP], cbh, 0);
        int32_t bo = used_len(cs->offset[CSS_BOTTOM], cbh, 0);
        int32_t vmbp = b->marg[CSS_TOP] + b->bord[CSS_TOP] + b->pad[CSS_TOP] +
                       b->pad[CSS_BOTTOM] + b->bord[CSS_BOTTOM] +
                       b->marg[CSS_BOTTOM];

        h = imax(0, cbh - t - bo - vmbp);
    }
    h = clamp_height(h, cs, cbh);
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

/* ================================================================== *
 * 11  post pass: clips, ink extents, paint order, node index
 * ================================================================== */

#define LAY_BIG 0x20000000

static void compute_clips(struct lay_document *L)
{
    struct lay_box *n = L->icb;

    n->clip_x = -LAY_BIG;
    n->clip_y = -LAY_BIG;
    n->clip_w = 2 * LAY_BIG;
    n->clip_h = 2 * LAY_BIG;
    for (;;) {
        if (n != L->icb && n->parent) {
            lay_rect c = lay_mkrect(n->parent->clip_x, n->parent->clip_y,
                                    n->parent->clip_w, n->parent->clip_h);

            if (n->flags & LAYF_CLIP)
                c = lay_rect_intersect(c, lay_padding_rect(n));
            n->clip_x = c.x;
            n->clip_y = c.y;
            n->clip_w = c.w;
            n->clip_h = c.h;
        } else if (n != L->icb) {
            n->clip_x = -LAY_BIG;
            n->clip_y = -LAY_BIG;
            n->clip_w = 2 * LAY_BIG;
            n->clip_h = 2 * LAY_BIG;
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

/* Post-order, iterative: a box's ink extent is its own border box united
 * with every descendant's, which is what lets a dirty-rectangle repaint
 * reject an entire subtree with one comparison. */
static void compute_ink(struct lay_document *L)
{
    struct lay_box *n = L->icb;
    int descending = 1;

    for (;;) {
        if (descending) {
            lay_rect own = lay_border_rect(n);

            if (n->kind == LAY_BOX_TEXT || n->kind == LAY_BOX_MARKER)
                own = lay_mkrect(n->x, n->y, n->w, n->h);
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

            if (p) {
                lay_rect u = lay_rect_union(lay_ink_rect(p), lay_ink_rect(n));
                lay_rect cl = lay_mkrect(p->clip_x, p->clip_y,
                                         p->clip_w, p->clip_h);

                if (p->flags & LAYF_CLIP)
                    u = lay_rect_intersect(u, lay_rect_union(
                            lay_border_rect(p), cl));
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
    unsigned long v = (unsigned long)(uintptr_t)p;

    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdUL;
    v ^= v >> 29;
    return v;
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

/* ---- hit testing ------------------------------------------------- */

struct lay_box *lay_hit_test(const struct lay_document *L, int32_t x, int32_t y)
{
    int i;

    if (!L || !L->paint)
        return 0;
    for (i = L->npaint - 1; i >= 0; i--) {
        struct lay_box *b = L->paint[i].box;
        lay_rect r;

        if (b->flags & LAYF_HIDDEN)
            continue;
        if (!lay_rect_contains(lay_clip_rect(b), x, y))
            continue;
        r = (b->kind == LAY_BOX_TEXT || b->kind == LAY_BOX_MARKER)
            ? lay_mkrect(b->x, b->y, b->w, b->h) : lay_border_rect(b);
        if (r.w <= 0 || r.h <= 0)
            continue;
        if (lay_rect_contains(r, x, y))
            return b;
    }
    return 0;
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
