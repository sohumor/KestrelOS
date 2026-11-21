/* test_layout.c - exhaustive host tests for libweb/layout.c and paint.c.
 *
 * The DOM and the computed styles are built directly here: no HTML
 * parser, no CSS parser, no cascade. That is deliberate. A layout bug
 * and a cascade bug look identical through a stylesheet, so every
 * geometric assertion below starts from a struct computed_style this
 * file filled in by hand, and the numbers are derived from the font
 * metrics in libgui/font.h rather than from what the code happens to do.
 *
 *   FONT_SMALL  6x12   ascent  9
 *   FONT_BODY   8x16   ascent 12   <- font-size 14..20 maps here
 *   FONT_LARGE  12x24  ascent 18
 *   FONT_HUGE   16x32  ascent 24
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -DPAINT_WITH_STDIO \
 *       -Ilibweb -Ilibgui -Ilibimg -o /tmp/tl tools/test_layout.c \
 *       libweb/layout.c libweb/paint.c libweb/dom.c libweb/html.c \
 *       libweb/entities.c libgui/font.c libgui/font_data.c
 *
 * Paint-order OOM gate: add -DLAYOUT_TEST_WRAP_ALLOC and
 * -Wl,--wrap=malloc, then run /tmp/tl --paint-oom.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dom.h"
#include "css.h"
#include "font.h"
#include "layout.h"
#include "paint.h"
#include "img.h"

/* ------------------------------------------------------------------ *
 * css_style_initial
 *
 * layout.c needs exactly one symbol from the CSS side. Rather than link
 * style.c - which another agent is still writing, and whose breakage
 * would look like a layout failure - the initial values are written out
 * here from CSS 2.1 directly. Building with -DLAYOUT_TEST_REAL_CSS links
 * the real one instead, which is how the two are checked against each
 * other; the two builds must produce identical results.
 * ------------------------------------------------------------------ */

#ifndef LAYOUT_TEST_REAL_CSS
void css_style_initial(struct computed_style *cs)
{
    int i;

    memset(cs, 0, sizeof *cs);
    cs->display = CSS_DISPLAY_INLINE;
    cs->position = CSS_POSITION_STATIC;
    cs->css_float = CSS_FLOAT_NONE;
    cs->clear = CSS_CLEAR_NONE;
    cs->text_align = CSS_TEXTALIGN_START;
    cs->vertical_align = CSS_VALIGN_BASELINE;
    cs->white_space = CSS_WHITESPACE_NORMAL;
    cs->overflow = CSS_OVERFLOW_VISIBLE;
    cs->visibility = CSS_VISIBILITY_VISIBLE;
    cs->font_style = CSS_FONTSTYLE_NORMAL;
    cs->font_family = CSS_FONTFAMILY_SERIF;
    cs->list_style_type = CSS_LISTSTYLE_DISC;
    cs->list_style_position = CSS_LISTPOS_OUTSIDE;
    cs->border_collapse = CSS_BORDERCOLLAPSE_SEPARATE;
    cs->text_transform = CSS_TEXTTRANSFORM_NONE;
    cs->text_decoration = CSS_DECOR_NONE;
    cs->flex_direction = CSS_FLEXDIR_ROW;
    cs->flex_wrap = CSS_FLEXWRAP_NOWRAP;
    cs->justify_content = CSS_JUSTIFY_START;
    cs->align_items = CSS_ALIGN_STRETCH;
    cs->box_sizing = CSS_BOXSIZING_CONTENT;
    cs->color_scheme = CSS_COLORSCHEME_NORMAL;
    cs->font_weight = 400;
    cs->opacity = 1000;
    cs->font_size = 16;
    cs->z_auto = 1;
    cs->flex_shrink = 1000;
    cs->color = CSS_RGB(0, 0, 0);
    cs->background_color = CSS_TRANSPARENT;
    cs->width.type = CSS_LEN_AUTO;
    cs->height.type = CSS_LEN_AUTO;
    cs->min_width.type = CSS_LEN_PX;
    cs->min_height.type = CSS_LEN_PX;
    cs->max_width.type = CSS_LEN_NONE;
    cs->max_height.type = CSS_LEN_NONE;
    cs->flex_basis.type = CSS_LEN_AUTO;
    cs->line_height.type = CSS_LEN_NORMAL;
    cs->text_indent.type = CSS_LEN_PX;
    cs->grad_angle_deg = 180;
    cs->grid_col_span = 1;
    cs->grid_row_span = 1;
    cs->align_self = CSS_ALIGNSELF_AUTO;
    cs->object_fit = CSS_OBJECTFIT_FILL;
    cs->outline_style = CSS_BORDERSTYLE_NONE;
    cs->outline_color = CSS_RGB(0, 0, 0);
    cs->text_overflow = CSS_TEXTOVERFLOW_CLIP;
    cs->word_break = CSS_WORDBREAK_NORMAL;
    cs->overflow_wrap = CSS_OVERFLOWWRAP_NORMAL;
    cs->scale_x = 1000;
    cs->scale_y = 1000;
    cs->translate_x.type = CSS_LEN_PX;
    cs->translate_y.type = CSS_LEN_PX;
    for (i = 0; i < 4; i++) {
        cs->margin[i].type = CSS_LEN_PX;
        cs->padding[i].type = CSS_LEN_PX;
        cs->offset[i].type = CSS_LEN_AUTO;
        cs->border_style[i] = CSS_BORDERSTYLE_NONE;
        cs->border_color[i] = CSS_RGB(0, 0, 0);
        cs->border_width[i] = 0;
        cs->border_radius[i] = 0;
    }
}
#endif

/* ------------------------------------------------------------------ *
 * harness
 * ------------------------------------------------------------------ */

static long checks, failures;
static const char *group = "";
static int stack_gate_mode;

#ifdef LAYOUT_TEST_WRAP_ALLOC
/*
 * Host-only malloc fault injection.  Tracking is armed immediately before
 * lay_layout_node(), after the fixture and its styles have been allocated.
 * A successful calibration run identifies the final four malloc calls:
 * build_paint_order()'s positioned-box, key, scratch, and published-order
 * arrays.  The fixed trace is allocation-free so it cannot perturb itself.
 */
#define OOM_TRACE_MAX 256
static int oom_tracking;
static unsigned long oom_calls;
static unsigned long oom_fail_call;
static unsigned long oom_failures;
static size_t oom_sizes[OOM_TRACE_MAX];

void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
    unsigned long call;

    if (oom_tracking) {
        call = ++oom_calls;
        if (call <= OOM_TRACE_MAX)
            oom_sizes[call - 1] = size;
        if (oom_fail_call && call == oom_fail_call) {
            oom_failures++;
            return 0;
        }
    }
    return __real_malloc(size);
}

static void oom_begin(unsigned long fail_call)
{
    memset(oom_sizes, 0, sizeof oom_sizes);
    oom_calls = 0;
    oom_fail_call = fail_call;
    oom_failures = 0;
    oom_tracking = 1;
}

static unsigned long oom_end(void)
{
    oom_tracking = 0;
    return oom_calls;
}
#endif

#define CHECK(cond) do {                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("  FAIL [%s] %s:%d: %s\n", group, __FILE__,          \
                   __LINE__, #cond);                                    \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b) do {                                             \
        intptr_t _a = (intptr_t)(a), _b = (intptr_t)(b);                \
        checks++;                                                       \
        if (_a != _b) {                                                 \
            failures++;                                                 \
            printf("  FAIL [%s] %s:%d: %s == %s  (%lld vs %lld)\n",     \
                   group, __FILE__, __LINE__, #a, #b,                   \
                   (long long)_a, (long long)_b);                       \
        }                                                               \
    } while (0)

#define GROUP(name) do { group = (name); printf("%s\n", (name)); } while (0)

/* ------------------------------------------------------------------ *
 * building documents by hand
 * ------------------------------------------------------------------ */

#define POOL_MAX 8192

struct tctx {
    struct dom_document *doc;
    struct computed_style *pool[POOL_MAX];
    int npool;
};

static struct tctx *tnew(void)
{
    struct tctx *t = calloc(1, sizeof *t);

    t->doc = dom_document_new();
    return t;
}

static void tfree(struct tctx *t)
{
    int i;

    for (i = 0; i < t->npool; i++)
        free(t->pool[i]);
    dom_document_free(t->doc);
    free(t);
}

static struct computed_style *st(struct tctx *t)
{
    struct computed_style *cs = calloc(1, sizeof *cs);

    css_style_initial(cs);
    if (t->npool < POOL_MAX)
        t->pool[t->npool++] = cs;
    return cs;
}

static struct computed_style *st_block(struct tctx *t)
{
    struct computed_style *cs = st(t);

    cs->display = CSS_DISPLAY_BLOCK;
    return cs;
}

static struct dom_node *el(struct tctx *t, struct dom_node *parent,
                           const char *tag, struct computed_style *cs)
{
    struct dom_node *n = dom_create_element(t->doc, tag, -1);

    n->style = cs;
    if (parent)
        dom_append_child(parent, n);
    return n;
}

static struct dom_node *txt(struct tctx *t, struct dom_node *parent,
                            const char *s)
{
    struct dom_node *n = dom_create_text(t->doc, s, strlen(s));

    dom_append_child(parent, n);
    return n;
}

static struct css_len LPX(int32_t v)
{
    struct css_len l;

    l.v = v;
    l.type = CSS_LEN_PX;
    return l;
}

static struct css_len LPCT(int32_t pct)
{
    struct css_len l;

    l.v = pct * 1000;
    l.type = CSS_LEN_PCT;
    return l;
}

static struct css_len LAUTO(void)
{
    struct css_len l;

    l.v = 0;
    l.type = CSS_LEN_AUTO;
    return l;
}

static void set_margin(struct computed_style *cs, int32_t t, int32_t r,
                       int32_t b, int32_t l)
{
    cs->margin[CSS_TOP] = LPX(t);
    cs->margin[CSS_RIGHT] = LPX(r);
    cs->margin[CSS_BOTTOM] = LPX(b);
    cs->margin[CSS_LEFT] = LPX(l);
}

static void set_padding(struct computed_style *cs, int32_t t, int32_t r,
                        int32_t b, int32_t l)
{
    cs->padding[CSS_TOP] = LPX(t);
    cs->padding[CSS_RIGHT] = LPX(r);
    cs->padding[CSS_BOTTOM] = LPX(b);
    cs->padding[CSS_LEFT] = LPX(l);
}

static void set_border(struct computed_style *cs, int32_t w, uint32_t color)
{
    int i;

    for (i = 0; i < 4; i++) {
        cs->border_width[i] = w;
        cs->border_style[i] = w ? CSS_BORDERSTYLE_SOLID : CSS_BORDERSTYLE_NONE;
        cs->border_color[i] = color;
    }
}

static struct lay_document *run(struct tctx *t, struct dom_node *root, int w,
                                int h)
{
    struct lay_opts o;

    (void)t;
    lay_opts_init(&o, w, h);
    return lay_layout_node(root, &o);
}

/* Find the nth box generated by a node. */
struct nth {
    int want, seen;
    struct lay_box *hit;
};

static void nth_cb(void *ctx, struct lay_box *b)
{
    struct nth *n = ctx;

    if (n->seen++ == n->want)
        n->hit = b;
}

static struct lay_box *box_nth(struct lay_document *L, struct dom_node *n,
                               int idx)
{
    struct nth s;

    s.want = idx;
    s.seen = 0;
    s.hit = 0;
    lay_boxes_for_node(L, n, nth_cb, &s);
    return s.hit;
}

/* First line box of a block. */
static struct lay_box *first_line(struct lay_box *b)
{
    struct lay_box *c;

    if (!b)
        return 0;
    for (c = b->first_child; c; c = c->next)
        if (c->kind == LAY_BOX_LINE)
            return c;
    return 0;
}

static int count_lines(struct lay_box *b)
{
    struct lay_box *c;
    int n = 0;

    for (c = b ? b->first_child : 0; c; c = c->next)
        if (c->kind == LAY_BOX_LINE)
            n++;
    return n;
}

/* First text box anywhere under b, in document order. */
static struct lay_box *first_text(struct lay_box *b)
{
    struct lay_box *n = b;

    if (!b)
        return 0;
    for (;;) {
        if (n->kind == LAY_BOX_TEXT)
            return n;
        if (n->first_child) { n = n->first_child; continue; }
        while (n != b && !n->next) n = n->parent;
        if (n == b) return 0;
        n = n->next;
    }
}

static struct lay_box *nth_text(struct lay_box *b, int idx)
{
    struct lay_box *n = b;
    int seen = 0;

    if (!b)
        return 0;
    for (;;) {
        if (n->kind == LAY_BOX_TEXT && seen++ == idx)
            return n;
        if (n->first_child) { n = n->first_child; continue; }
        while (n != b && !n->next) n = n->parent;
        if (n == b) return 0;
        n = n->next;
    }
}

/* ================================================================== *
 * 1  the box model
 * ================================================================== */

static void test_box_model(void)
{
    struct tctx *t = tnew();
    struct computed_style *outer = st_block(t), *inner = st_block(t);
    struct dom_node *o, *i;
    struct lay_document *L;
    struct lay_box *bo, *bi;

    GROUP("box model: content/padding/border/margin");

    set_margin(outer, 10, 10, 10, 10);
    set_padding(outer, 5, 5, 5, 5);
    set_border(outer, 2, CSS_RGB(0, 0, 0));
    outer->height = LPX(100);

    set_margin(inner, 0, 0, 0, 8);
    set_padding(inner, 4, 4, 4, 4);
    set_border(inner, 1, CSS_RGB(0, 0, 0));
    inner->height = LPX(20);

    o = el(t, 0, "div", outer);
    i = el(t, o, "div", inner);

    L = run(t, o, 400, 300);
    bo = box_nth(L, o, 0);
    bi = box_nth(L, i, 0);

    /* outer: margin 10, border 2, padding 5 -> content origin (17,17) */
    CHECK_EQ(bo->x, 17);
    CHECK_EQ(bo->y, 17);
    CHECK_EQ(bo->w, 400 - 2 * (10 + 2 + 5));
    CHECK_EQ(bo->h, 100);
    CHECK_EQ(lay_border_rect(bo).x, 10);
    CHECK_EQ(lay_border_rect(bo).w, 400 - 20);
    CHECK_EQ(lay_margin_rect(bo).x, 0);
    CHECK_EQ(lay_margin_rect(bo).w, 400);
    CHECK_EQ(lay_padding_rect(bo).x, 12);

    /* inner: inside outer content, margin-left 8, border 1, padding 4 */
    CHECK_EQ(bi->x, 17 + 8 + 1 + 4);
    CHECK_EQ(bi->y, 17 + 1 + 4);
    CHECK_EQ(bi->w, bo->w - 8 - 2 * (1 + 4));
    CHECK_EQ(bi->h, 20);

    lay_free(L);
    tfree(t);
}

static void test_over_constrained(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("box model: over-constrained width drops margin-right");

    cs->width = LPX(100);
    set_margin(cs, 0, 50, 0, 20);
    d = el(t, 0, "div", cs);
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);

    CHECK_EQ(b->w, 100);
    CHECK_EQ(b->marg[CSS_LEFT], 20);
    CHECK_EQ(b->marg[CSS_RIGHT], 400 - 100 - 20);   /* not 50 */
    CHECK_EQ(b->x, 20);
    lay_free(L);
    tfree(t);
}

static void test_centred(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("box model: margin 0 auto centres a fixed-width block");

    cs->width = LPX(300);
    cs->margin[CSS_LEFT] = LAUTO();
    cs->margin[CSS_RIGHT] = LAUTO();
    d = el(t, 0, "div", cs);
    L = run(t, d, 1000, 300);
    b = box_nth(L, d, 0);

    CHECK_EQ(b->w, 300);
    CHECK_EQ(b->x, 350);
    CHECK_EQ(b->marg[CSS_LEFT], 350);
    CHECK_EQ(b->marg[CSS_RIGHT], 350);
    lay_free(L);

    /* with a border and padding the centring still uses the border box */
    set_border(cs, 5, CSS_RGB(0, 0, 0));
    set_padding(cs, 0, 10, 0, 10);
    L = run(t, d, 1000, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(lay_border_rect(b).w, 300 + 2 * 10 + 2 * 5);
    CHECK_EQ(lay_border_rect(b).x, (1000 - (300 + 20 + 10)) / 2);
    lay_free(L);

    /* one auto margin takes the whole remainder */
    cs->margin[CSS_LEFT] = LAUTO();
    cs->margin[CSS_RIGHT] = LPX(100);
    L = run(t, d, 1000, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(b->marg[CSS_LEFT], 1000 - 330 - 100);
    lay_free(L);
    tfree(t);
}

static void test_percent_width(void)
{
    struct tctx *t = tnew();
    struct computed_style *a = st_block(t), *b2 = st_block(t);
    struct dom_node *o, *i;
    struct lay_document *L;
    struct lay_box *bo, *bi;

    GROUP("box model: percentage widths and padding");

    a->width = LPCT(50);
    b2->width = LPCT(50);
    b2->padding[CSS_LEFT] = LPCT(10);   /* % padding is of the CB WIDTH */
    a->height = LPX(50);
    b2->height = LPX(10);

    o = el(t, 0, "div", a);
    i = el(t, o, "div", b2);
    L = run(t, o, 800, 300);
    bo = box_nth(L, o, 0);
    bi = box_nth(L, i, 0);

    CHECK_EQ(bo->w, 400);
    CHECK_EQ(bi->w, 200);
    CHECK_EQ(bi->pad[CSS_LEFT], 40);
    CHECK_EQ(bi->x, bo->x + 40);
    lay_free(L);
    tfree(t);
}

static void test_percent_height(void)
{
    struct tctx *t = tnew();
    struct computed_style *outer = st_block(t), *inner = st_block(t);
    struct dom_node *o, *i;
    struct lay_document *L;

    GROUP("box model: percentage height needs a definite containing height");

    outer->height = LPX(200);
    inner->height = LPCT(50);
    o = el(t, 0, "div", outer);
    i = el(t, o, "div", inner);
    txt(t, i, "one line");

    L = run(t, o, 400, 300);
    CHECK_EQ(box_nth(L, i, 0)->h, 100);
    lay_free(L);

    outer->height = LAUTO();
    L = run(t, o, 400, 300);
    CHECK_EQ(box_nth(L, i, 0)->h, 16);
    lay_free(L);
    tfree(t);
}

static void test_min_max(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("box model: min-width and max-width clamping");

    cs->max_width = LPX(200);
    cs->margin[CSS_LEFT] = LAUTO();
    cs->margin[CSS_RIGHT] = LAUTO();
    d = el(t, 0, "div", cs);
    L = run(t, d, 900, 300);
    b = box_nth(L, d, 0);
    /* auto width clamped to max, then the auto margins recentre it */
    CHECK_EQ(b->w, 200);
    CHECK_EQ(b->x, 350);
    lay_free(L);

    cs->max_width.type = CSS_LEN_NONE;
    cs->min_width = LPX(600);
    cs->width = LPX(100);
    L = run(t, d, 900, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(b->w, 600);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 2  margin collapsing
 * ================================================================== */

static void test_collapse_siblings(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct computed_style *a = st_block(t), *b = st_block(t);
    struct dom_node *dp, *da, *db;
    struct lay_document *L;
    struct lay_box *ba, *bb, *bp;

    GROUP("margin collapsing: adjacent siblings take the larger margin");

    set_border(p, 1, CSS_RGB(0, 0, 0));   /* stop parent/child collapsing */
    a->height = LPX(20);
    b->height = LPX(20);
    set_margin(a, 0, 0, 30, 0);
    set_margin(b, 20, 0, 0, 0);

    dp = el(t, 0, "div", p);
    da = el(t, dp, "div", a);
    db = el(t, dp, "div", b);
    L = run(t, dp, 400, 300);
    bp = box_nth(L, dp, 0);
    ba = box_nth(L, da, 0);
    bb = box_nth(L, db, 0);

    CHECK_EQ(ba->y, 1);
    CHECK_EQ(bb->y, 1 + 20 + 30);      /* max(30, 20), not 50 */
    CHECK_EQ(bp->h, 20 + 30 + 20);
    lay_free(L);

    /* negative margins: max positive plus min negative */
    set_margin(a, 0, 0, 30, 0);
    set_margin(b, -10, 0, 0, 0);
    L = run(t, dp, 400, 300);
    ba = box_nth(L, da, 0);
    bb = box_nth(L, db, 0);
    CHECK_EQ(bb->y - (ba->y + 20), 20);   /* 30 + (-10) */
    lay_free(L);

    set_margin(a, 0, 0, -30, 0);
    set_margin(b, -10, 0, 0, 0);
    L = run(t, dp, 400, 300);
    ba = box_nth(L, da, 0);
    bb = box_nth(L, db, 0);
    CHECK_EQ(bb->y - (ba->y + 20), -30);  /* min(-30, -10) */
    lay_free(L);
    tfree(t);
}

static void test_collapse_parent_child(void)
{
    struct tctx *t = tnew();
    struct computed_style *g = st_block(t), *p = st_block(t);
    struct computed_style *c = st_block(t);
    struct dom_node *dg, *dp, *dc;
    struct lay_document *L;
    struct lay_box *bg, *bp, *bc;

    GROUP("margin collapsing: parent and first child");

    set_border(g, 1, CSS_RGB(0, 0, 0));
    set_margin(p, 10, 0, 10, 0);
    set_margin(c, 40, 0, 40, 0);
    c->height = LPX(20);

    dg = el(t, 0, "div", g);
    dp = el(t, dg, "div", p);
    dc = el(t, dp, "div", c);
    L = run(t, dg, 400, 300);
    bg = box_nth(L, dg, 0);
    bp = box_nth(L, dp, 0);
    bc = box_nth(L, dc, 0);

    /* the two top margins collapse into one 40, applied above BOTH */
    CHECK_EQ(bp->y, 1 + 40);
    CHECK_EQ(bc->y, 1 + 40);
    /* the two bottom margins collapse too, so the parent's height is
     * exactly the child's border box */
    CHECK_EQ(bp->h, 20);
    CHECK_EQ(bg->h, 40 + 20 + 40);
    lay_free(L);

    /* a border on the parent stops the collapse in both directions */
    set_border(p, 3, CSS_RGB(0, 0, 0));
    L = run(t, dg, 400, 300);
    bp = box_nth(L, dp, 0);
    bc = box_nth(L, dc, 0);
    CHECK_EQ(lay_border_rect(bp).y, 1 + 10);
    CHECK_EQ(bc->y, 1 + 10 + 3 + 40);
    CHECK_EQ(bp->h, 40 + 20 + 40);
    lay_free(L);

    /* so does padding */
    set_border(p, 0, CSS_RGB(0, 0, 0));
    set_padding(p, 7, 0, 7, 0);
    L = run(t, dg, 400, 300);
    bp = box_nth(L, dp, 0);
    bc = box_nth(L, dc, 0);
    CHECK_EQ(lay_border_rect(bp).y, 1 + 10);
    CHECK_EQ(bc->y, 1 + 10 + 7 + 40);
    lay_free(L);
    tfree(t);
}

static void test_collapse_through(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *a = st_block(t);
    struct computed_style *e = st_block(t), *b = st_block(t);
    struct dom_node *dp, *da, *db;
    struct lay_document *L;
    struct lay_box *ba, *bb, *bp;

    GROUP("margin collapsing: an empty box collapses through");

    set_border(p, 1, CSS_RGB(0, 0, 0));
    a->height = LPX(20);
    b->height = LPX(20);
    set_margin(a, 0, 0, 20, 0);
    set_margin(e, 10, 0, 10, 0);      /* empty, no height, no borders */
    set_margin(b, 20, 0, 0, 0);

    dp = el(t, 0, "div", p);
    da = el(t, dp, "div", a);
    (void)el(t, dp, "div", e);
    db = el(t, dp, "div", b);
    L = run(t, dp, 400, 300);
    ba = box_nth(L, da, 0);
    bb = box_nth(L, db, 0);
    bp = box_nth(L, dp, 0);

    /* all four margins are adjoining: max(20,10,10,20) = 20 */
    CHECK_EQ(bb->y - (ba->y + 20), 20);
    CHECK_EQ(bp->h, 20 + 20 + 20);
    lay_free(L);

    /* give the empty box a height and it stops collapsing through */
    e->height = LPX(5);
    L = run(t, dp, 400, 300);
    ba = box_nth(L, da, 0);
    bb = box_nth(L, db, 0);
    CHECK_EQ(bb->y - (ba->y + 20), 20 + 5 + 20);
    lay_free(L);
    tfree(t);
}

static void test_collapse_bfc(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *c = st_block(t);
    struct dom_node *dp, *dc;
    struct lay_document *L;
    struct lay_box *bp, *bc;

    GROUP("margin collapsing: a new formatting context does not collapse");

    p->overflow = CSS_OVERFLOW_HIDDEN;
    set_margin(c, 30, 0, 30, 0);
    c->height = LPX(10);
    dp = el(t, 0, "div", p);
    dc = el(t, dp, "div", c);
    L = run(t, dp, 400, 300);
    bp = box_nth(L, dp, 0);
    bc = box_nth(L, dc, 0);

    CHECK_EQ(bp->y, 0);
    CHECK_EQ(bc->y, 30);
    CHECK_EQ(bp->h, 30 + 10 + 30);
    CHECK(bp->flags & LAYF_BFC);
    CHECK(bp->flags & LAYF_CLIP);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 3  inline formatting
 * ================================================================== */

static void test_text_wrap(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b, *l1, *l2, *tx;

    GROUP("inline: wrapping at exact widths, 8x16 face");

    d = el(t, 0, "div", p);
    txt(t, d, "hello world");

    /* wide enough for both words: 5*8 + 8 + 5*8 = 88 */
    p->width = LPX(200);
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 1);
    l1 = first_line(b);
    CHECK_EQ(l1->w, 88);
    CHECK_EQ(l1->h, 16);          /* line-height: normal on 8x16 */
    CHECK_EQ(l1->baseline, 12);   /* the face's ascent */
    CHECK_EQ(b->h, 16);
    lay_free(L);

    /* exactly 88 still fits */
    p->width = LPX(88);
    L = run(t, d, 400, 300);
    CHECK_EQ(count_lines(box_nth(L, d, 0)), 1);
    lay_free(L);

    /* one pixel less and it wraps */
    p->width = LPX(87);
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 2);
    l1 = first_line(b);
    l2 = l1->next;
    CHECK_EQ(l1->w, 40);
    CHECK_EQ(l2->w, 40);
    CHECK_EQ(l1->y, 0);
    CHECK_EQ(l2->y, 16);
    CHECK_EQ(b->h, 32);
    tx = first_text(l1);
    CHECK_EQ(tx->text_len, 5);
    CHECK(!memcmp(tx->text, "hello", 5));
    CHECK_EQ(tx->x, 0);
    CHECK_EQ(tx->w, 40);
    CHECK_EQ(tx->y, 0);
    CHECK_EQ(tx->h, 16);
    tx = first_text(l2);
    CHECK(!memcmp(tx->text, "world", 5));
    lay_free(L);

    /* a word wider than the line is broken by character */
    p->width = LPX(24);
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 4);     /* hel/lo/wor/ld */
    CHECK_EQ(first_text(first_line(b))->text_len, 3);
    lay_free(L);
    tfree(t);
}

static void test_line_height(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b, *l;

    GROUP("inline: line-height drives the line box height");

    d = el(t, 0, "div", p);
    txt(t, d, "one two three four five six seven");
    p->width = LPX(100);

    p->line_height.type = CSS_LEN_PX;
    p->line_height.v = 30;
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);
    l = first_line(b);
    CHECK_EQ(l->h, 30);
    /* half-leading: (30-16)/2 = 7 above the face, so baseline 12+7 */
    CHECK_EQ(l->baseline, 19);
    CHECK_EQ(b->h, 30 * count_lines(b));
    /* the glyph cell still sits on the baseline */
    CHECK_EQ(first_text(l)->y, l->y + 19 - 12);
    lay_free(L);

    p->line_height.type = CSS_LEN_NUMBER;
    p->line_height.v = 2000;      /* 2.0 x font-size 16 = 32 */
    L = run(t, d, 400, 300);
    CHECK_EQ(first_line(box_nth(L, d, 0))->h, 32);
    lay_free(L);

    p->line_height.type = CSS_LEN_PCT;
    p->line_height.v = 150 * 1000;   /* 150% of 16 = 24 */
    L = run(t, d, 400, 300);
    CHECK_EQ(first_line(box_nth(L, d, 0))->h, 24);
    lay_free(L);
    tfree(t);
}

static void test_white_space(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("inline: white-space normal/pre/nowrap/pre-wrap/pre-line");

    d = el(t, 0, "div", p);
    txt(t, d, "  a   b  \n  c  ");
    p->width = LPX(400);

    p->white_space = CSS_WHITESPACE_NORMAL;
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 1);
    /* "a b c": leading and trailing space dropped, runs collapsed */
    CHECK_EQ(first_line(b)->w, 8 * 5);
    lay_free(L);

    p->white_space = CSS_WHITESPACE_PRE;
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 2);           /* the newline is preserved */
    CHECK_EQ(first_line(b)->w, 8 * 9);     /* "  a   b  " */
    lay_free(L);

    /* pre does not wrap, however narrow the block is */
    p->width = LPX(20);
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 2);
    CHECK_EQ(first_line(b)->w, 8 * 9);
    lay_free(L);

    p->white_space = CSS_WHITESPACE_NOWRAP;
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 1);
    CHECK_EQ(first_line(b)->w, 8 * 5);     /* collapsed but unwrapped */
    lay_free(L);

    p->white_space = CSS_WHITESPACE_PRE_LINE;
    p->width = LPX(400);
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 2);           /* newline kept, spaces not */
    CHECK_EQ(first_line(b)->w, 8 * 3);     /* "a b" */
    lay_free(L);

    p->white_space = CSS_WHITESPACE_PRE_WRAP;
    p->width = LPX(40);
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK(count_lines(b) >= 3);            /* preserved AND wrapped */
    lay_free(L);
    tfree(t);
}

static void test_text_align(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("inline: text-align and text-indent");

    d = el(t, 0, "div", p);
    txt(t, d, "hello");
    p->width = LPX(200);

    L = run(t, d, 400, 300);
    CHECK_EQ(first_line(box_nth(L, d, 0))->x, 0);
    lay_free(L);

    p->text_align = CSS_TEXTALIGN_RIGHT;
    L = run(t, d, 400, 300);
    CHECK_EQ(first_line(box_nth(L, d, 0))->x, 200 - 40);
    lay_free(L);

    p->text_align = CSS_TEXTALIGN_CENTER;
    L = run(t, d, 400, 300);
    CHECK_EQ(first_line(box_nth(L, d, 0))->x, (200 - 40) / 2);
    lay_free(L);

    p->text_align = CSS_TEXTALIGN_LEFT;
    p->text_indent = LPX(24);
    L = run(t, d, 400, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(first_text(b)->x, 24);
    CHECK_EQ(first_line(b)->w, 64);
    lay_free(L);
    tfree(t);
}

static void test_inline_boxes(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *s = st(t);
    struct dom_node *d, *sp;
    struct lay_document *L;
    struct lay_box *b, *ib;
    int frags;

    GROUP("inline: an inline box with padding, split across lines");

    p->width = LPX(400);
    s->display = CSS_DISPLAY_INLINE;
    set_padding(s, 0, 6, 0, 6);
    s->border_width[CSS_LEFT] = s->border_width[CSS_RIGHT] = 2;
    s->border_style[CSS_LEFT] = s->border_style[CSS_RIGHT] =
        CSS_BORDERSTYLE_SOLID;

    d = el(t, 0, "div", p);
    txt(t, d, "aa");
    sp = el(t, d, "span", s);
    txt(t, sp, "bb");
    txt(t, d, "cc");

    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    ib = box_nth(L, sp, 0);
    CHECK_EQ(count_lines(b), 1);
    CHECK(ib != 0);
    CHECK(ib->flags & LAYF_FIRST_FRAG);
    CHECK(ib->flags & LAYF_LAST_FRAG);
    /* "aa" is 16 wide; the span opens with border 2 + padding 6 */
    CHECK_EQ(ib->x, 16 + 2 + 6);
    CHECK_EQ(ib->w, 16);
    CHECK_EQ(lay_border_rect(ib).w, 16 + 2 * (2 + 6));
    /* the line ends after the span's right edge plus "cc" */
    CHECK_EQ(first_line(b)->w, 16 + 2 + 6 + 16 + 6 + 2 + 16);
    lay_free(L);

    /* narrow it so the span straddles a line break */
    p->width = LPX(40);
    d = el(t, 0, "div", p);
    txt(t, d, "one");
    sp = el(t, d, "span", s);
    txt(t, sp, "two three");
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    frags = lay_boxes_for_node(L, sp, 0, 0);
    CHECK(frags >= 2);                       /* one fragment per line */
    CHECK(box_nth(L, sp, 0)->flags & LAYF_FIRST_FRAG);
    CHECK(!(box_nth(L, sp, 0)->flags & LAYF_LAST_FRAG));
    CHECK(box_nth(L, sp, frags - 1)->flags & LAYF_LAST_FRAG);
    CHECK(!(box_nth(L, sp, frags - 1)->flags & LAYF_FIRST_FRAG));
    lay_free(L);
    tfree(t);
}

static void test_vertical_align(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *big = st(t);
    struct dom_node *d, *sp;
    struct lay_document *L;
    struct lay_box *b, *l, *t0, *t1;

    GROUP("inline: vertical-align baseline/top/bottom/middle");

    p->width = LPX(600);
    big->display = CSS_DISPLAY_INLINE;
    big->font_size = 32;              /* the 16x32 face: ascent 24 */

    d = el(t, 0, "div", p);
    txt(t, d, "x");
    sp = el(t, d, "span", big);
    txt(t, sp, "Y");

    /* baseline: the line grows to 24 above + max(4, 8) below */
    L = run(t, d, 700, 300);
    b = box_nth(L, d, 0);
    l = first_line(b);
    CHECK_EQ(l->baseline, 24);
    CHECK_EQ(l->h, 32);
    t0 = nth_text(l, 0);
    t1 = nth_text(l, 1);
    CHECK_EQ(t0->y + 12, l->y + 24);   /* small face sits on the baseline */
    CHECK_EQ(t1->y + 24, l->y + 24);
    lay_free(L);

    big->vertical_align = CSS_VALIGN_TOP;
    L = run(t, d, 700, 300);
    l = first_line(box_nth(L, d, 0));
    CHECK_EQ(nth_text(l, 1)->y, l->y);
    lay_free(L);

    big->vertical_align = CSS_VALIGN_BOTTOM;
    L = run(t, d, 700, 300);
    l = first_line(box_nth(L, d, 0));
    CHECK_EQ(nth_text(l, 1)->y + 32, l->y + l->h);
    lay_free(L);

    big->vertical_align = CSS_VALIGN_MIDDLE;
    L = run(t, d, 700, 300);
    l = first_line(box_nth(L, d, 0));
    CHECK(nth_text(l, 1)->y >= l->y);
    CHECK(nth_text(l, 1)->y + 32 <= l->y + l->h);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 4  floats
 * ================================================================== */

static void test_floats(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *fl = st_block(t);
    struct computed_style *cl = st_block(t);
    struct dom_node *d, *f, *c;
    struct lay_document *L;
    struct lay_box *b, *bf, *bc, *l1, *l2;

    GROUP("floats: line boxes shorten beside a float, clear escapes it");

    p->width = LPX(400);
    fl->css_float = CSS_FLOAT_LEFT;
    fl->width = LPX(100);
    fl->height = LPX(48);           /* exactly three 16px lines */
    d = el(t, 0, "div", p);
    f = el(t, d, "div", fl);
    txt(t, d, "aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll");
    c = el(t, d, "div", cl);
    txt(t, c, "mmm nnn ooo ppp qqq rrr sss ttt uuu vvv www xxx "
              "yyy zzz aaa bbb ccc ddd eee fff");

    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    bf = box_nth(L, f, 0);
    bc = box_nth(L, c, 0);

    CHECK_EQ(bf->x, 0);
    CHECK_EQ(bf->y, 0);
    CHECK_EQ(bf->w, 100);
    CHECK_EQ(bf->h, 48);

    /* the anonymous block holding the text starts at y 0 and its first
     * lines are pushed right by the float and are 300 wide */
    {
        struct lay_box *anon = 0, *k;

        for (k = b->first_child; k; k = k->next)
            if (k->kind == LAY_BOX_BLOCK && (k->flags & LAYF_IFC))
                { anon = k; break; }
        CHECK(anon != 0);
        l1 = anon ? first_line(anon) : 0;
        CHECK(l1 != 0);
        if (l1) {
            CHECK_EQ(l1->x, 100);
            CHECK(l1->w <= 300);
        }
    }

    /* An ordinary block starts in normal flow even while the float is
     * present. Its first line avoids the float; once below y=48, its next
     * line gets the full containing-block width. */
    CHECK_EQ(bc->y, 32);
    l1 = first_line(bc);
    CHECK(l1 != 0);
    if (l1) {
        CHECK_EQ(l1->x, 100);
        CHECK(l1->w <= 300);
        for (l2 = l1; l2 && l2->y < 48; l2 = l2->next)
            ;
        CHECK(l2 != 0);
        if (l2) {
            CHECK_EQ(l2->x, 0);
            CHECK(l2->w > 300);
            CHECK(l2->w <= 400);
        }
    }
    lay_free(L);

    /* an explicit clear on a block that would otherwise fit beside */
    cl->height = LPX(10);
    d = el(t, 0, "div", p);
    f = el(t, d, "div", fl);
    c = el(t, d, "div", cl);
    L = run(t, d, 500, 300);
    bc = box_nth(L, c, 0);
    CHECK_EQ(bc->y, 0);              /* blocks ignore floats by default */
    lay_free(L);

    cl->clear = CSS_CLEAR_LEFT;
    L = run(t, d, 500, 300);
    bc = box_nth(L, c, 0);
    CHECK_EQ(bc->y, 48);
    lay_free(L);

    /* a right float shortens the line from the other side */
    fl->css_float = CSS_FLOAT_RIGHT;
    cl->clear = CSS_CLEAR_NONE;
    d = el(t, 0, "div", p);
    f = el(t, d, "div", fl);
    txt(t, d, "aaa bbb ccc");
    L = run(t, d, 500, 300);
    bf = box_nth(L, f, 0);
    CHECK_EQ(bf->x, 300);
    {
        struct lay_box *flow = box_nth(L, d, 0), *k;

        if (!(flow->flags & LAYF_IFC)) {
            flow = 0;
            for (k = box_nth(L, d, 0)->first_child; k; k = k->next)
                if (k->kind == LAY_BOX_BLOCK && (k->flags & LAYF_IFC))
                    { flow = k; break; }
        }
        CHECK(flow != 0);
        if (flow && first_line(flow)) {
            CHECK_EQ(first_line(flow)->x, 0);
            CHECK(first_line(flow)->w <= 300);
        }
    }
    lay_free(L);
    tfree(t);
}

static void test_float_containment(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *fl = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("floats: a block formatting context contains its floats");

    p->width = LPX(400);
    fl->css_float = CSS_FLOAT_LEFT;
    fl->width = LPX(50);
    fl->height = LPX(80);

    d = el(t, 0, "div", p);
    (void)el(t, d, "div", fl);
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(b->h, 0);           /* a plain block does not contain it */
    lay_free(L);

    p->overflow = CSS_OVERFLOW_HIDDEN;
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(b->h, 80);          /* ... a BFC root does */
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 5  lists
 * ================================================================== */

static void test_lists(void)
{
    struct tctx *t = tnew();
    struct computed_style *ul = st_block(t), *li = st(t);
    struct dom_node *u, *a, *b2, *c;
    struct lay_document *L;
    struct lay_box *ba, *m;

    GROUP("lists: markers outside and inside, decimal/alpha/roman");

    ul->width = LPX(300);
    ul->padding[CSS_LEFT] = LPX(40);
    li->display = CSS_DISPLAY_LIST_ITEM;
    li->list_style_type = CSS_LISTSTYLE_DECIMAL;

    u = el(t, 0, "ul", ul);
    a = el(t, u, "li", li);
    txt(t, a, "one");
    b2 = el(t, u, "li", li);
    txt(t, b2, "two");
    c = el(t, u, "li", li);
    txt(t, c, "three");

    L = run(t, u, 400, 300);
    ba = box_nth(L, a, 0);
    for (m = ba->first_child; m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    CHECK(m != 0);
    CHECK(m->marker != 0);
    CHECK(!strcmp(m->marker, "1."));
    /* "1." is 2 glyphs of 8 plus a half-cell gap */
    CHECK_EQ(m->w, 20);
    CHECK_EQ(m->x, ba->x - 20);
    CHECK_EQ(m->y + 12, first_line(ba)->y + first_line(ba)->baseline);

    ba = box_nth(L, c, 0);
    for (m = ba->first_child; m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    CHECK(m && !strcmp(m->marker, "3."));
    lay_free(L);

    li->list_style_type = CSS_LISTSTYLE_LOWER_ROMAN;
    L = run(t, u, 400, 300);
    ba = box_nth(L, c, 0);
    for (m = ba->first_child; m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    CHECK(m && !strcmp(m->marker, "iii."));
    lay_free(L);

    li->list_style_type = CSS_LISTSTYLE_UPPER_ALPHA;
    L = run(t, u, 400, 300);
    ba = box_nth(L, c, 0);
    for (m = ba->first_child; m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    CHECK(m && !strcmp(m->marker, "C."));
    lay_free(L);

    /* a bullet has no text: the painter draws the glyph */
    li->list_style_type = CSS_LISTSTYLE_DISC;
    L = run(t, u, 400, 300);
    ba = box_nth(L, a, 0);
    for (m = ba->first_child; m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    CHECK(m && m->marker == 0);
    CHECK_EQ(m->w, 12);
    lay_free(L);

    /* inside: the marker joins the first line instead */
    li->list_style_type = CSS_LISTSTYLE_DECIMAL;
    li->list_style_position = CSS_LISTPOS_INSIDE;
    L = run(t, u, 400, 300);
    ba = box_nth(L, a, 0);
    CHECK_EQ(first_line(ba)->x, ba->x);
    CHECK_EQ(first_line(ba)->w, 20 + 3 * 8);
    lay_free(L);
    tfree(t);
}

static struct lay_box *marker_for(struct lay_document *L,
                                  struct dom_node *item)
{
    struct lay_box *b = box_nth(L, item, 0);
    struct lay_box *m;

    for (m = b ? b->first_child : 0;
         m && m->kind != LAY_BOX_MARKER; m = m->next)
        ;
    return m;
}

static void test_list_value_continuation(void)
{
    struct tctx *t = tnew();
    struct computed_style *ol = st_block(t), *li = st(t);
    struct dom_node *list, *a, *b, *c, *d;
    struct lay_document *L;

    GROUP("lists: ordered numbering continues after an explicit li value");

    li->display = CSS_DISPLAY_LIST_ITEM;
    li->list_style_type = CSS_LISTSTYLE_DECIMAL;
    list = el(t, 0, "ol", ol);
    dom_set_attr(list, "start", "3");
    a = el(t, list, "li", li);
    b = el(t, list, "li", li);
    dom_set_attr(b, "value", "10");
    c = el(t, list, "li", li);
    d = el(t, list, "li", li);

    L = run(t, list, 400, 300);
    CHECK(marker_for(L, a) && !strcmp(marker_for(L, a)->marker, "3."));
    CHECK(marker_for(L, b) && !strcmp(marker_for(L, b)->marker, "10."));
    CHECK(marker_for(L, c) && !strcmp(marker_for(L, c)->marker, "11."));
    CHECK(marker_for(L, d) && !strcmp(marker_for(L, d)->marker, "12."));
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 6  tables
 * ================================================================== */

static struct computed_style *st_disp(struct tctx *t, int disp)
{
    struct computed_style *cs = st(t);

    cs->display = disp;
    return cs;
}

static void test_table_auto(void)
{
    struct tctx *t = tnew();
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct computed_style *td = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct dom_node *tab, *r1, *r2, *c0, *c1, *c2;
    struct lay_document *L;
    struct lay_box *bt, *b0, *b1, *b2;

    GROUP("tables: automatic layout, colspan, uneven columns");

    tab = el(t, 0, "table", tb);
    r1 = el(t, tab, "tr", tr);
    c0 = el(t, r1, "td", td);
    dom_set_attr(c0, "colspan", "2");
    txt(t, c0, "abcdefgh");                /* one 64px word */
    r2 = el(t, tab, "tr", tr);
    c1 = el(t, r2, "td", td);
    txt(t, c1, "ab");                      /* 16 */
    c2 = el(t, r2, "td", td);
    txt(t, c2, "cdef");                    /* 32 */

    L = run(t, tab, 400, 300);
    bt = box_nth(L, tab, 0);
    b0 = box_nth(L, c0, 0);
    b1 = box_nth(L, c1, 0);
    b2 = box_nth(L, c2, 0);

    /* shrink to fit: the widest thing is the 64px spanning cell */
    CHECK_EQ(bt->w, 64);
    CHECK_EQ(b0->w, 64);
    /* the deficit is shared in proportion to the columns' own maxima:
     * 16 + 16*16/48 = 21 and 32 + 16*32/48 = 42, +1 rounding = 43 */
    CHECK_EQ(b1->w, 21);
    CHECK_EQ(b2->w, 43);
    CHECK_EQ(b1->w + b2->w, b0->w);
    CHECK_EQ(b1->x, bt->x);
    CHECK_EQ(b2->x, bt->x + 21);
    /* two rows of one 16px line each */
    CHECK_EQ(bt->h, 32);
    CHECK_EQ(b0->y, bt->y);
    CHECK_EQ(b1->y, bt->y + 16);
    lay_free(L);

    /* a declared width is distributed over the same columns */
    tb->width = LPX(200);
    L = run(t, tab, 400, 300);
    bt = box_nth(L, tab, 0);
    b0 = box_nth(L, c0, 0);
    b1 = box_nth(L, c1, 0);
    b2 = box_nth(L, c2, 0);
    CHECK_EQ(bt->w, 200);
    CHECK_EQ(b0->w, 200);
    CHECK_EQ(b1->w + b2->w, 200);
    CHECK(b2->w > b1->w);
    lay_free(L);
    tfree(t);
}

static void test_table_spacing_and_span(void)
{
    struct tctx *t = tnew();
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct computed_style *td = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct dom_node *tab, *r1, *r2, *r3, *a, *b, *c, *d;
    struct lay_document *L;
    struct lay_box *bt, *ba, *bb, *bc;

    GROUP("tables: border-spacing, cell padding, rowspan");

    tb->border_spacing = 4;
    set_padding(td, 2, 3, 2, 3);

    tab = el(t, 0, "table", tb);
    r1 = el(t, tab, "tr", tr);
    a = el(t, r1, "td", td);
    dom_set_attr(a, "rowspan", "2");
    txt(t, a, "tall");
    b = el(t, r1, "td", td);
    txt(t, b, "one");
    r2 = el(t, tab, "tr", tr);
    c = el(t, r2, "td", td);
    txt(t, c, "two");
    r3 = el(t, tab, "tr", tr);
    d = el(t, r3, "td", td);
    txt(t, d, "x");
    (void)d;

    L = run(t, tab, 400, 300);
    bt = box_nth(L, tab, 0);
    ba = box_nth(L, a, 0);
    bb = box_nth(L, b, 0);
    bc = box_nth(L, c, 0);

    /* spacing frames every cell: first cell starts one spacing in */
    CHECK_EQ(lay_border_rect(ba).x, bt->x + 4);
    CHECK_EQ(lay_border_rect(ba).y, bt->y + 4);
    CHECK_EQ(ba->pad[CSS_LEFT], 3);
    CHECK_EQ(lay_border_rect(bb).x,
             lay_border_rect(ba).x + lay_border_rect(ba).w + 4);
    /* the rowspan cell covers both of the first two rows */
    CHECK_EQ(lay_border_rect(ba).h,
             lay_border_rect(bb).h + 4 + lay_border_rect(bc).h);
    CHECK_EQ(lay_border_rect(bc).y,
             lay_border_rect(bb).y + lay_border_rect(bb).h + 4);
    lay_free(L);
    tfree(t);
}

static void test_table_fixed(void)
{
    struct tctx *t = tnew();
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct computed_style *td1 = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct computed_style *td2 = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct dom_node *tab, *r1, *r2, *a, *b, *c, *d;
    struct lay_document *L;
    struct lay_box *ba, *bb;

    GROUP("tables: fixed layout takes the first row's declared widths");

    tb->width = LPX(300);
    td1->width = LPX(100);
    td2->width = LPX(200);

    tab = el(t, 0, "table", tb);
    r1 = el(t, tab, "tr", tr);
    a = el(t, r1, "td", td1);
    txt(t, a, "a");
    b = el(t, r1, "td", td2);
    txt(t, b, "b");
    r2 = el(t, tab, "tr", tr);
    c = el(t, r2, "td", td1);
    txt(t, c, "a very long piece of text that would otherwise widen this");
    d = el(t, r2, "td", td2);
    txt(t, d, "d");
    (void)d;

    L = run(t, tab, 600, 300);
    ba = box_nth(L, a, 0);
    bb = box_nth(L, b, 0);
    CHECK_EQ(box_nth(L, tab, 0)->w, 300);
    CHECK_EQ(ba->w, 100);
    CHECK_EQ(bb->w, 200);
    /* the long second-row cell wraps rather than widening the column */
    CHECK_EQ(box_nth(L, c, 0)->w, 100);
    CHECK(count_lines(box_nth(L, c, 0)) > 1);
    lay_free(L);
    tfree(t);
}

static void test_table_row_index_capacity(void)
{
    GROUP("tables: configured row limit fits the public zero-based row index");

    CHECK((unsigned long)LAY_MAX_ROWS <=
          (unsigned long)UINT16_MAX + 1UL);
}

static void test_table_row_boundary_runtime(void)
{
    struct tctx *t = tnew();
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct dom_node *tab, *overflow = 0;
    struct lay_document *L;
    struct lay_box *bt, *g, *r;
    int i, rows = 0, exact = 1;

    GROUP("tables: row indices remain unique through the uint16 boundary");

    tab = el(t, 0, "table", tb);
    for (i = 0; i <= LAY_MAX_ROWS; i++) {
        struct dom_node *n = el(t, tab, "tr", tr);

        if (i == LAY_MAX_ROWS)
            overflow = n;
    }

    L = run(t, tab, 100, 100);
    CHECK(L != 0);
    bt = L ? box_nth(L, tab, 0) : 0;
    for (g = bt ? bt->first_child : 0; g; g = g->next) {
        if (g->kind != LAY_BOX_TABLE_ROWGROUP)
            continue;
        for (r = g->first_child; r; r = r->next) {
            if (r->kind != LAY_BOX_TABLE_ROW)
                continue;
            if (rows > (int)UINT16_MAX || r->row != (uint16_t)rows)
                exact = 0;
            rows++;
        }
    }
    CHECK_EQ(rows, LAY_MAX_ROWS);
    CHECK(exact);
    CHECK(L && (lay_truncated(L) & LAY_TRUNC_TABLE));
    CHECK(L && box_nth(L, overflow, 0) == 0);
    if (L)
        lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 7  replaced elements
 * ================================================================== */

static int fake_image_size(void *ctx, const char *url, int *w, int *h)
{
    (void)ctx;
    if (!strcmp(url, "a.png")) { *w = 100; *h = 50; return 1; }
    if (!strcmp(url, "sq.png")) { *w = 64; *h = 64; return 1; }
    return 0;
}

static void test_replaced(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *im = st(t);
    struct computed_style *media = st(t);
    struct dom_node *d, *i, *v, *a, *svg, *svg_ratio;
    struct lay_document *L;
    struct lay_opts o;
    struct lay_box *b;

    GROUP("replaced: intrinsic size and aspect ratio");

    p->width = LPX(400);
    im->display = CSS_DISPLAY_INLINE;
    d = el(t, 0, "div", p);
    i = el(t, d, "img", im);
    dom_set_attr(i, "src", "a.png");

    lay_opts_init(&o, 500, 300);
    o.image_size = fake_image_size;

    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 100);
    CHECK_EQ(b->h, 50);
    lay_free(L);

    /* width only: the height follows the intrinsic ratio */
    im->width = LPX(200);
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 200);
    CHECK_EQ(b->h, 100);
    lay_free(L);

    /* height only */
    im->width = LAUTO();
    im->height = LPX(25);
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 50);
    CHECK_EQ(b->h, 25);
    lay_free(L);

    /* both given: the ratio is ignored */
    im->width = LPX(33);
    im->height = LPX(77);
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 33);
    CHECK_EQ(b->h, 77);
    lay_free(L);

    /* max-width clamps and the height follows it back down */
    im->width = LPX(200);
    im->height = LAUTO();
    im->max_width = LPX(50);
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 50);
    CHECK_EQ(b->h, 25);
    lay_free(L);

    /* no decoder and no attributes: alt text sizes the placeholder */
    im->width = LAUTO();
    im->max_width.type = CSS_LEN_NONE;
    dom_set_attr(i, "src", "missing.png");
    dom_set_attr(i, "alt", "photo");
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 5 * 8 + 8);
    CHECK_EQ(b->h, 16 + 4);
    lay_free(L);

    /* width/height attributes are used when the decoder has nothing */
    dom_remove_attr(i, "alt");
    dom_set_attr(i, "width", "80");
    dom_set_attr(i, "height", "40");
    L = lay_layout_node(d, &o);
    b = box_nth(L, i, 0);
    CHECK_EQ(b->w, 80);
    CHECK_EQ(b->h, 40);
    lay_free(L);

    /* Video posters use the ordinary image pipeline and preserve its
     * intrinsic ratio. Audio and inline SVG have HTML default sizes. */
    media->display = CSS_DISPLAY_INLINE_BLOCK;
    v = el(t, d, "video", media);
    dom_set_attr(v, "poster", "a.png");
    a = el(t, d, "audio", media);
    svg = el(t, d, "svg", media);
    svg_ratio = el(t, d, "svg", media);
    dom_set_attr(svg_ratio, "width", "120");
    dom_set_attr(svg_ratio, "viewBox", "0 0 4 2");
    L = lay_layout_node(d, &o);
    CHECK_EQ(box_nth(L, v, 0)->w, 100);
    CHECK_EQ(box_nth(L, v, 0)->h, 50);
    CHECK_EQ(box_nth(L, a, 0)->w, 300);
    CHECK_EQ(box_nth(L, a, 0)->h, 32);
    CHECK_EQ(box_nth(L, svg, 0)->w, 300);
    CHECK_EQ(box_nth(L, svg, 0)->h, 150);
    CHECK_EQ(box_nth(L, svg_ratio, 0)->w, 120);
    CHECK_EQ(box_nth(L, svg_ratio, 0)->h, 60);
    lay_free(L);
    tfree(t);
}

static void test_flex_row(void)
{
    struct tctx *t = tnew();
    struct computed_style *flex = st_block(t);
    struct computed_style *a = st_block(t);
    struct computed_style *b = st_block(t);
    struct computed_style *c = st_block(t);
    struct dom_node *root, *na, *nb, *nc;
    struct lay_document *L;
    struct lay_box *ba, *bb, *bc;

    GROUP("flex: row growth, gaps, and cross-axis centering");

    flex->display = CSS_DISPLAY_FLEX;
    flex->width = LPX(300);
    flex->gap = 10;
    flex->align_items = CSS_ALIGN_CENTER;
    a->width = LPX(50);
    a->height = LPX(10);
    b->height = LPX(20);
    b->flex_grow = 1000;
    c->height = LPX(30);
    c->flex_grow = 2000;
    root = el(t, 0, "div", flex);
    na = el(t, root, "div", a);
    nb = el(t, root, "div", b);
    nc = el(t, root, "div", c);

    L = run(t, root, 400, 200);
    ba = box_nth(L, na, 0);
    bb = box_nth(L, nb, 0);
    bc = box_nth(L, nc, 0);
    CHECK(ba && bb && bc);
    CHECK_EQ(box_nth(L, root, 0)->w, 300);
    CHECK_EQ(ba->x, 0);
    CHECK_EQ(ba->w, 50);
    CHECK_EQ(bb->x, 60);
    CHECK_EQ(bb->w, 76);
    CHECK_EQ(bc->x, 146);
    CHECK_EQ(bc->w, 154);
    CHECK_EQ(ba->y, 10);
    CHECK_EQ(bb->y, 5);
    CHECK_EQ(bc->y, 0);
    CHECK_EQ(box_nth(L, root, 0)->h, 30);
    lay_free(L);

    GROUP("flex: row-reverse preserves the allocated item sizes");
    flex->flex_direction = CSS_FLEXDIR_ROW_REVERSE;
    flex->align_items = CSS_ALIGN_START;
    L = run(t, root, 400, 200);
    ba = box_nth(L, na, 0);
    bb = box_nth(L, nb, 0);
    bc = box_nth(L, nc, 0);
    CHECK_EQ(bc->x, 0);
    CHECK_EQ(bc->w, 154);
    CHECK_EQ(bb->x, 164);
    CHECK_EQ(bb->w, 76);
    CHECK_EQ(ba->x, 250);
    CHECK_EQ(ba->w, 50);
    lay_free(L);
    tfree(t);
}

static void test_flex_column(void)
{
    struct tctx *t = tnew();
    struct computed_style *flex = st_block(t);
    struct computed_style *a = st_block(t);
    struct computed_style *b = st_block(t);
    struct computed_style *c = st_block(t);
    struct dom_node *root, *na, *nb, *nc;
    struct lay_document *L;
    struct lay_box *ba, *bb, *bc;

    GROUP("flex: column growth, gap, and cross-axis alignment");

    flex->display = CSS_DISPLAY_FLEX;
    flex->flex_direction = CSS_FLEXDIR_COLUMN;
    flex->width = LPX(200);
    flex->height = LPX(160);
    flex->gap = 10;
    flex->align_items = CSS_ALIGN_CENTER;
    a->width = b->width = c->width = LPX(50);
    a->height = b->height = c->height = LPX(20);
    b->flex_grow = 1000;
    c->flex_grow = 2000;
    root = el(t, 0, "div", flex);
    na = el(t, root, "div", a);
    nb = el(t, root, "div", b);
    nc = el(t, root, "div", c);

    L = run(t, root, 400, 300);
    ba = box_nth(L, na, 0);
    bb = box_nth(L, nb, 0);
    bc = box_nth(L, nc, 0);
    CHECK(ba && bb && bc);
    CHECK_EQ(ba->x, 75);
    CHECK_EQ(bb->x, 75);
    CHECK_EQ(bc->x, 75);
    CHECK_EQ(ba->y, 0);
    CHECK_EQ(ba->h, 20);
    CHECK_EQ(bb->y, 30);
    CHECK_EQ(bb->h, 46);
    CHECK_EQ(bc->y, 86);
    CHECK_EQ(bc->h, 74);
    CHECK_EQ(box_nth(L, root, 0)->h, 160);
    lay_free(L);

    GROUP("flex: column-reverse and justify-content");
    b->flex_grow = c->flex_grow = 0;
    flex->flex_direction = CSS_FLEXDIR_COLUMN_REVERSE;
    flex->justify_content = CSS_JUSTIFY_CENTER;
    L = run(t, root, 400, 300);
    ba = box_nth(L, na, 0);
    bb = box_nth(L, nb, 0);
    bc = box_nth(L, nc, 0);
    CHECK_EQ(bc->y, 40);
    CHECK_EQ(bb->y, 70);
    CHECK_EQ(ba->y, 100);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 8  positioning
 * ================================================================== */

static void test_absolute(void)
{
    struct tctx *t = tnew();
    struct computed_style *rel = st_block(t), *ab = st_block(t);
    struct computed_style *sib = st_block(t);
    struct dom_node *r, *a, *s;
    struct lay_document *L;
    struct lay_box *br, *ba, *bs;

    GROUP("positioning: absolute against the padding box of an ancestor");

    rel->position = CSS_POSITION_RELATIVE;
    rel->width = LPX(300);
    rel->height = LPX(200);
    set_padding(rel, 20, 20, 20, 20);
    set_border(rel, 5, CSS_RGB(0, 0, 0));
    set_margin(rel, 30, 0, 0, 40);

    ab->position = CSS_POSITION_ABSOLUTE;
    ab->offset[CSS_LEFT] = LPX(10);
    ab->offset[CSS_TOP] = LPX(15);
    ab->width = LPX(50);
    ab->height = LPX(60);

    sib->height = LPX(10);

    r = el(t, 0, "div", rel);
    a = el(t, r, "div", ab);
    s = el(t, r, "div", sib);

    L = run(t, r, 800, 600);
    br = box_nth(L, r, 0);
    ba = box_nth(L, a, 0);
    bs = box_nth(L, s, 0);

    /* the containing block is the PADDING box: border edge + border */
    CHECK_EQ(ba->x, lay_border_rect(br).x + 5 + 10);
    CHECK_EQ(ba->y, lay_border_rect(br).y + 5 + 15);
    CHECK_EQ(ba->w, 50);
    CHECK_EQ(ba->h, 60);
    /* it is out of flow: the sibling starts at the top of the content */
    CHECK_EQ(bs->y, br->y);
    lay_free(L);

    /* right + bottom */
    ab->offset[CSS_LEFT] = LAUTO();
    ab->offset[CSS_TOP] = LAUTO();
    ab->offset[CSS_RIGHT] = LPX(10);
    ab->offset[CSS_BOTTOM] = LPX(15);
    L = run(t, r, 800, 600);
    br = box_nth(L, r, 0);
    ba = box_nth(L, a, 0);
    CHECK_EQ(ba->x + 50, lay_padding_rect(br).x + lay_padding_rect(br).w - 10);
    CHECK_EQ(ba->y + 60, lay_padding_rect(br).y + lay_padding_rect(br).h - 15);
    lay_free(L);

    /* left + right with auto width stretches */
    ab->offset[CSS_LEFT] = LPX(10);
    ab->offset[CSS_RIGHT] = LPX(20);
    ab->offset[CSS_TOP] = LPX(0);
    ab->offset[CSS_BOTTOM] = LAUTO();
    ab->width = LAUTO();
    L = run(t, r, 800, 600);
    br = box_nth(L, r, 0);
    ba = box_nth(L, a, 0);
    CHECK_EQ(ba->w, lay_padding_rect(br).w - 10 - 20);
    lay_free(L);
    tfree(t);
}

static void test_fixed_and_relative(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *r = st_block(t);
    struct computed_style *fx = st_block(t);
    struct dom_node *dp, *dr, *df;
    struct lay_document *L;
    struct lay_box *br, *bf;

    GROUP("positioning: relative offsets and fixed to the viewport");

    p->height = LPX(500);
    r->position = CSS_POSITION_RELATIVE;
    r->offset[CSS_LEFT] = LPX(25);
    r->offset[CSS_TOP] = LPX(-8);
    r->height = LPX(20);

    fx->position = CSS_POSITION_FIXED;
    fx->offset[CSS_RIGHT] = LPX(0);
    fx->offset[CSS_BOTTOM] = LPX(0);
    fx->width = LPX(40);
    fx->height = LPX(40);

    dp = el(t, 0, "div", p);
    dr = el(t, dp, "div", r);
    df = el(t, dp, "div", fx);

    L = run(t, dp, 600, 400);
    br = box_nth(L, dr, 0);
    bf = box_nth(L, df, 0);
    CHECK_EQ(br->x, 25);
    CHECK_EQ(br->y, -8);
    CHECK_EQ(bf->x, 600 - 40);
    CHECK_EQ(bf->y, 400 - 40);
    lay_free(L);
    tfree(t);
}

static void test_fixed_inside_relative(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *r = st_block(t);
    struct computed_style *fx = st_block(t);
    struct dom_node *dp, *dr, *df;
    struct lay_document *L;
    struct lay_box *bf;

    GROUP("positioning: fixed descendant ignores relative ancestor offset");

    p->height = LPX(500);
    r->position = CSS_POSITION_RELATIVE;
    r->offset[CSS_LEFT] = LPX(25);
    r->offset[CSS_TOP] = LPX(30);
    r->height = LPX(20);
    fx->position = CSS_POSITION_FIXED;
    fx->offset[CSS_RIGHT] = LPX(0);
    fx->offset[CSS_BOTTOM] = LPX(0);
    fx->width = LPX(40);
    fx->height = LPX(40);

    dp = el(t, 0, "div", p);
    dr = el(t, dp, "div", r);
    df = el(t, dr, "div", fx);
    L = run(t, dp, 600, 400);
    bf = box_nth(L, df, 0);
    CHECK_EQ(bf->x, 600 - 40);
    CHECK_EQ(bf->y, 400 - 40);
    lay_free(L);
    tfree(t);
}

static void test_zorder(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct computed_style *lo = st_block(t), *hi = st_block(t);
    struct dom_node *d, *a, *b;
    struct lay_document *L;
    const struct lay_paint_item *items;
    int n, i, ia = -1, ib = -1;

    GROUP("positioning: paint order follows z-index");

    p->height = LPX(200);
    lo->position = CSS_POSITION_ABSOLUTE;
    lo->z_auto = 0;
    lo->z_index = 5;
    lo->width = LPX(50);
    lo->height = LPX(50);
    hi->position = CSS_POSITION_ABSOLUTE;
    hi->z_auto = 0;
    hi->z_index = 1;
    hi->width = LPX(50);
    hi->height = LPX(50);

    d = el(t, 0, "div", p);
    a = el(t, d, "div", lo);      /* z 5, first in the source */
    b = el(t, d, "div", hi);      /* z 1, second */

    L = run(t, d, 400, 400);
    n = lay_paint_order(L, &items);
    for (i = 0; i < n; i++) {
        if (items[i].box->node == a && ia < 0) ia = i;
        if (items[i].box->node == b && ib < 0) ib = i;
    }
    CHECK(ia >= 0 && ib >= 0);
    CHECK(ib < ia);              /* z 1 paints below z 5 */
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 9  overflow, hit testing, ids
 * ================================================================== */

static void test_overflow(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *c = st_block(t);
    struct dom_node *d, *i;
    struct lay_document *L;
    struct lay_box *bp, *bi;

    GROUP("overflow: clip rectangles and the scrollable extent");

    p->width = LPX(100);
    p->height = LPX(50);
    p->overflow = CSS_OVERFLOW_AUTO;
    c->width = LPX(300);
    c->height = LPX(400);

    d = el(t, 0, "div", p);
    i = el(t, d, "div", c);
    L = run(t, d, 400, 300);
    bp = box_nth(L, d, 0);
    bi = box_nth(L, i, 0);

    CHECK(bp->flags & LAYF_SCROLL);
    CHECK_EQ(bp->scroll_w, 300);
    CHECK_EQ(bp->scroll_h, 400);
    /* the child's effective clip is the parent's padding box */
    CHECK_EQ(bi->clip_x, lay_padding_rect(bp).x);
    CHECK_EQ(bi->clip_w, lay_padding_rect(bp).w);
    CHECK_EQ(bi->clip_h, 50);
    /* the parent's ink extent does not leak past its own box */
    CHECK(bp->ink_h <= 50);
    lay_free(L);
    tfree(t);
}

static void test_hit_test(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *as = st(t);
    struct dom_node *d, *a;
    struct lay_document *L;
    const char *href = 0;

    GROUP("hit testing: point -> box -> node -> link");

    p->width = LPX(400);
    as->display = CSS_DISPLAY_INLINE;
    d = el(t, 0, "div", p);
    txt(t, d, "go ");
    a = el(t, d, "a", as);
    dom_set_attr(a, "href", "http://example.com/");
    txt(t, a, "here");
    txt(t, d, " now");

    L = run(t, d, 500, 300);

    /* "go " is 3 cells; the link occupies x 24..56 on the first line */
    CHECK(lay_hit_test(L, 30, 8) != 0);
    CHECK_EQ((intptr_t)lay_link_at(L, 30, 8, &href), (intptr_t)a);
    CHECK(href && !strcmp(href, "http://example.com/"));
    CHECK_EQ((intptr_t)lay_link_at(L, 8, 8, 0), 0);
    CHECK_EQ((intptr_t)lay_link_at(L, 70, 8, 0), 0);
    CHECK_EQ((intptr_t)lay_link_at(L, 30, 900, 0), 0);
    CHECK(lay_node_at(L, 30, 8) != 0);

    /* clipped content is not hit */
    lay_free(L);

    p->overflow = CSS_OVERFLOW_HIDDEN;
    p->height = LPX(4);
    L = run(t, d, 500, 300);
    CHECK_EQ((intptr_t)lay_hit_test(L, 30, 10), 0);
    lay_free(L);
    tfree(t);
}

static void test_scroll_to_id(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *c = st_block(t);
    struct dom_node *d, *a, *b;
    struct lay_document *L;
    int32_t y = -1;

    GROUP("fragment scrolling: id -> document y");

    c->height = LPX(120);
    d = el(t, 0, "div", p);
    a = el(t, d, "div", c);
    b = el(t, d, "div", c);
    dom_set_attr(b, "id", "target");
    (void)a;

    L = run(t, d, 400, 300);
    CHECK_EQ(lay_scroll_to_id(L, "target", &y), 1);
    CHECK_EQ(y, 120);
    CHECK_EQ(lay_scroll_to_id(L, "nope", &y), 0);
    lay_free(L);
    tfree(t);
}

static void test_inline_block(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *ib = st(t);
    struct dom_node *d, *i;
    struct lay_document *L;
    struct lay_box *b, *bi;

    GROUP("inline-block: shrink to fit and sit on the line");

    p->width = LPX(400);
    ib->display = CSS_DISPLAY_INLINE_BLOCK;
    set_padding(ib, 4, 4, 4, 4);

    d = el(t, 0, "div", p);
    txt(t, d, "ab");
    i = el(t, d, "span", ib);
    txt(t, i, "xyz");

    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    bi = box_nth(L, i, 0);
    CHECK(bi->flags & LAYF_ATOMIC);
    CHECK(bi->flags & LAYF_BFC);
    CHECK_EQ(bi->w, 24);                     /* shrink to fit "xyz" */
    CHECK_EQ(bi->h, 16);
    CHECK_EQ(lay_border_rect(bi).x, 16);     /* just after "ab" */
    CHECK_EQ(first_line(b)->h, 24);          /* 16 content + 8 padding */
    lay_free(L);

    /* an inline-block wider than the space available wraps to its own line */
    p->width = LPX(30);
    L = run(t, d, 500, 300);
    b = box_nth(L, d, 0);
    CHECK_EQ(count_lines(b), 2);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 10  painting
 * ================================================================== */

static struct paint_target mk_target(int w, int h)
{
    struct paint_target t;

    t.w = w;
    t.h = h;
    t.stride = w;
    t.px = calloc((size_t)w * h, sizeof(uint32_t));
    return t;
}

static uint32_t px_at(const struct paint_target *t, int x, int y)
{
    if (x < 0 || y < 0 || x >= t->w || y >= t->h)
        return 0;
    return t->px[(long)y * t->stride + x] & 0x00FFFFFFu;
}

static long count_color(const struct paint_target *t, uint32_t c)
{
    long n = 0, i;

    for (i = 0; i < (long)t->w * t->h; i++)
        if ((t->px[i] & 0x00FFFFFFu) == c)
            n++;
    return n;
}

static void test_paint_svg(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st(t);
    struct dom_node *svg, *rect, *circle, *line, *polygon, *path, *bad_color;
    struct dom_node *zero_line, *multi, *svg_aspect, *aspect_rect;
    struct dom_node *svg_hostile, *huge_circle, *relative_path;
    struct lay_document *L;
    struct paint_target tg = mk_target(100, 80);
    struct paint_opts o;
    struct paint_stats stats;

    GROUP("paint: bounded inline SVG shapes and viewBox scaling");

    cs->display = CSS_DISPLAY_INLINE_BLOCK;
    svg = el(t, 0, "svg", cs);
    dom_set_attr(svg, "width", "100");
    dom_set_attr(svg, "height", "80");
    dom_set_attr(svg, "viewBox", "0 0 100 80");
    rect = el(t, svg, "rect", cs);
    dom_set_attr(rect, "x", "10");
    dom_set_attr(rect, "y", "10");
    dom_set_attr(rect, "width", "50");
    dom_set_attr(rect, "height", "20");
    dom_set_attr(rect, "fill", "#ff0000");
    circle = el(t, svg, "circle", cs);
    dom_set_attr(circle, "cx", "80");
    dom_set_attr(circle, "cy", "50");
    dom_set_attr(circle, "r", "10");
    dom_set_attr(circle, "fill", "blue");
    line = el(t, svg, "line", cs);
    dom_set_attr(line, "x1", "5");
    dom_set_attr(line, "y1", "70");
    dom_set_attr(line, "x2", "95");
    dom_set_attr(line, "y2", "70");
    dom_set_attr(line, "stroke", "green");
    dom_set_attr(line, "stroke-width", "2");
    polygon = el(t, svg, "polygon", cs);
    dom_set_attr(polygon, "points", "5,40 25,40 15,60");
    dom_set_attr(polygon, "style", "fill:#ff00ff");
    path = el(t, svg, "path", cs);
    dom_set_attr(path, "d", "M 60 10 L 90 10 L 75 30 Z");
    dom_set_attr(path, "style", "fill:#00ffff;stroke:#000000;"
                              "stroke-width:1");
    bad_color = el(t, svg, "rect", cs);
    dom_set_attr(bad_color, "x", "1");
    dom_set_attr(bad_color, "y", "1");
    dom_set_attr(bad_color, "width", "3");
    dom_set_attr(bad_color, "height", "3");
    dom_set_attr(bad_color, "fill", "#");
    zero_line = el(t, svg, "line", cs);
    dom_set_attr(zero_line, "x1", "10");
    dom_set_attr(zero_line, "y1", "75");
    dom_set_attr(zero_line, "x2", "90");
    dom_set_attr(zero_line, "y2", "75");
    dom_set_attr(zero_line, "stroke", "red");
    dom_set_attr(zero_line, "stroke-width", "0");
    multi = el(t, svg, "path", cs);
    dom_set_attr(multi, "d", "M 30 65 L 40 65 M 60 65 L 70 65");
    dom_set_attr(multi, "style", "fill:none;stroke:black;"
                               "stroke-width:1");

    L = run(t, svg, 100, 80);
    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &tg, lay_mkrect(0, 0, 100, 80), &o, &stats);
    CHECK_EQ(px_at(&tg, 15, 15), 0xFF0000);
    CHECK_EQ(px_at(&tg, 80, 50), 0x0000FF);
    CHECK_EQ(px_at(&tg, 50, 70), 0x008000);
    CHECK_EQ(px_at(&tg, 15, 45), 0xFF00FF);
    CHECK_EQ(px_at(&tg, 75, 15), 0x00FFFF);
    CHECK_EQ(px_at(&tg, 2, 2), 0x000000);
    CHECK_EQ(px_at(&tg, 35, 65), 0x000000);
    CHECK_EQ(px_at(&tg, 50, 65), 0xFFFFFF);
    CHECK_EQ(px_at(&tg, 50, 75), 0xFFFFFF);
    CHECK_EQ(stats.images, 1);
    free(tg.px);
    lay_free(L);

    svg_aspect = el(t, 0, "svg", cs);
    dom_set_attr(svg_aspect, "width", "100");
    dom_set_attr(svg_aspect, "height", "100");
    dom_set_attr(svg_aspect, "viewBox", "0 0 200 100");
    aspect_rect = el(t, svg_aspect, "rect", cs);
    dom_set_attr(aspect_rect, "width", "200");
    dom_set_attr(aspect_rect, "height", "100");
    dom_set_attr(aspect_rect, "fill", "red");
    L = run(t, svg_aspect, 100, 100);
    tg = mk_target(100, 100);
    lay_paint(L, &tg, lay_mkrect(0, 0, 100, 100), &o, &stats);
    CHECK_EQ(px_at(&tg, 50, 10), 0xFFFFFF);
    CHECK_EQ(px_at(&tg, 50, 50), 0xFF0000);
    free(tg.px);
    lay_free(L);

    svg_hostile = el(t, 0, "svg", cs);
    dom_set_attr(svg_hostile, "width", "10");
    dom_set_attr(svg_hostile, "height", "10");
    dom_set_attr(svg_hostile, "viewBox", "0 0 .001 .001");
    huge_circle = el(t, svg_hostile, "circle", cs);
    dom_set_attr(huge_circle, "cx", "0");
    dom_set_attr(huge_circle, "cy", "0");
    dom_set_attr(huge_circle, "r", "1000000");
    dom_set_attr(huge_circle, "fill", "blue");
    relative_path = el(t, svg_hostile, "path", cs);
    dom_set_attr(relative_path, "d",
                 "M0 0 l1000000 0 l1000000 0 l1000000 0");
    dom_set_attr(relative_path, "style", "fill:none;stroke:none");
    L = run(t, svg_hostile, 10, 10);
    tg = mk_target(10, 10);
    lay_paint(L, &tg, lay_mkrect(0, 0, 10, 10), &o, &stats);
    CHECK_EQ(px_at(&tg, 0, 0), 0x0000FF);
    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_basics(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *c = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct paint_target tg = mk_target(200, 120);
    struct paint_opts o;
    struct paint_stats stats;

    GROUP("paint: backgrounds, borders, clipping to the dirty rectangle");

    p->width = LPX(200);
    p->height = LPX(120);
    p->background_color = CSS_RGB(0xEE, 0xEE, 0xEE);
    p->overflow = CSS_OVERFLOW_HIDDEN; /* keep child margin from collapsing */
    c->width = LPX(60);
    c->height = LPX(40);
    set_margin(c, 20, 0, 0, 30);
    c->background_color = CSS_RGB(0x00, 0x80, 0x00);
    set_border(c, 3, CSS_RGB(0xFF, 0x00, 0x00));

    d = el(t, 0, "div", p);
    (void)el(t, d, "div", c);
    L = run(t, d, 200, 120);

    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 120), &o, &stats);

    /* the green content box, and the red border around it */
    CHECK_EQ(px_at(&tg, 40, 30), 0x008000);
    CHECK_EQ(px_at(&tg, 30, 21), 0xFF0000);      /* left border column */
    CHECK_EQ(px_at(&tg, 5, 5), 0xEEEEEE);        /* the page background */
    CHECK_EQ(count_color(&tg, 0x008000), 60 * 40);
    CHECK_EQ(count_color(&tg, 0xFF0000),
             (60 + 6) * (40 + 6) - 60 * 40);
    CHECK(stats.items_drawn > 0);

    /* an incremental repaint touches only what intersects the strip */
    {
        struct paint_stats s2;
        struct paint_target t2 = mk_target(200, 120);

        o.canvas = 0;                    /* no full-surface clear */
        lay_paint(L, &t2, lay_mkrect(0, 100, 200, 20), &o, &s2);
        CHECK(s2.items_drawn < stats.items_drawn);
        CHECK_EQ(px_at(&t2, 40, 30), 0);        /* outside the strip */
        CHECK_EQ(px_at(&t2, 40, 110), 0xEEEEEE);/* inside it */
        free(t2.px);
    }
    lay_free(L);

    /* With overflow visible again, the first child's top margin collapses
     * through the borderless, padding-free parent. Keep that geometry
     * separate from the background-painting assertion above. */
    p->overflow = CSS_OVERFLOW_VISIBLE;
    L = run(t, d, 200, 120);
    CHECK_EQ(box_nth(L, d, 0)->y, 20);

    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_clip(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t), *c = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct paint_target tg = mk_target(120, 120);
    struct paint_opts o;

    GROUP("paint: overflow hidden clips its descendants");

    p->width = LPX(60);
    p->height = LPX(30);
    p->overflow = CSS_OVERFLOW_HIDDEN;
    c->width = LPX(100);
    c->height = LPX(100);
    c->background_color = CSS_RGB(0x00, 0x00, 0xFF);

    d = el(t, 0, "div", p);
    (void)el(t, d, "div", c);
    L = run(t, d, 120, 120);

    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &tg, lay_mkrect(0, 0, 120, 120), &o, 0);

    CHECK_EQ(px_at(&tg, 10, 10), 0x0000FF);
    CHECK_EQ(px_at(&tg, 10, 40), 0xFFFFFF);   /* clipped below 30 */
    CHECK_EQ(px_at(&tg, 80, 10), 0xFFFFFF);   /* clipped past 60 */
    CHECK_EQ(count_color(&tg, 0x0000FF), 60 * 30);
    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_text_and_scroll(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct paint_target a = mk_target(200, 40), b = mk_target(200, 40);
    struct paint_opts o;
    int i, same = 1;

    GROUP("paint: scrolling is a translation, text is drawn once");

    p->width = LPX(200);
    p->color = CSS_RGB(0, 0, 0);
    d = el(t, 0, "div", p);
    txt(t, d, "one two three four five six seven eight nine ten");
    L = run(t, d, 200, 40);

    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &a, lay_mkrect(0, 0, 200, 40), &o, 0);
    o.scroll_y = 16;
    lay_paint(L, &b, lay_mkrect(0, 0, 200, 40), &o, 0);

    /* row 16 of the unscrolled render equals row 0 of the scrolled one */
    for (i = 0; i < 200; i++)
        if (px_at(&a, i, 16 + 3) != px_at(&b, i, 3))
            same = 0;
    CHECK(same);
    CHECK(count_color(&a, 0x000000) > 100);   /* glyphs actually drew */
    free(a.px);
    free(b.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_selection_caret(void)
{
    struct tctx *t = tnew();
    struct computed_style *p = st_block(t);
    struct dom_node *d;
    struct lay_document *L;
    struct paint_target tg = mk_target(200, 30);
    struct paint_opts o;
    struct lay_box *tb;

    GROUP("paint: the selection and caret hooks");

    p->width = LPX(200);
    d = el(t, 0, "div", p);
    txt(t, d, "abcdefgh");
    L = run(t, d, 200, 30);
    tb = first_text(box_nth(L, d, 0));

    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    o.caret_box = tb;
    o.caret_offset = 3;
    o.caret_color = CSS_RGB(0xFF, 0x00, 0x00);
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 30), &o, 0);
    CHECK_EQ(count_color(&tg, 0xFF0000), 16);   /* one 1x16 column */
    CHECK_EQ(px_at(&tg, 24, 4), 0xFF0000);
    free(tg.px);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 11  rendering whole pages to PPM
 * ================================================================== */

static void shot(struct lay_document *L, const char *name, int w, int h)
{
    struct paint_target tg = mk_target(w, h);
    struct paint_opts o;
    struct paint_stats s;
    char path[256];

    paint_opts_init(&o);
    o.canvas = lay_canvas_color(L);
    o.image = 0;
    lay_paint(L, &tg, lay_mkrect(0, 0, w, h), &o, &s);
    snprintf(path, sizeof path, "/tmp/layout-shots/%s.ppm", name);
    paint_write_ppm(&tg, path);
    printf("  shot %-14s %dx%d  boxes=%lu drawn=%lu glyphs=%lu\n",
           name, w, h, lay_box_count(L), s.items_drawn, s.glyphs);
    free(tg.px);
}

/* A document that exercises most of the engine at once. */
static struct dom_node *build_article(struct tctx *t)
{
    struct computed_style *body = st_block(t);
    struct computed_style *h1 = st_block(t), *h2 = st_block(t);
    struct computed_style *para = st_block(t);
    struct computed_style *quote = st_block(t);
    struct computed_style *ul = st_block(t), *li = st(t);
    struct computed_style *a = st(t), *code = st(t);
    struct computed_style *fl = st_block(t);
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct computed_style *th = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct computed_style *td = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct dom_node *b, *n, *p2, *tab, *r;
    int i;

    body->width = LAUTO();
    body->background_color = CSS_RGB(0xFF, 0xFF, 0xFF);
    body->color = CSS_RGB(0x1A, 0x1A, 0x1A);
    set_padding(body, 24, 24, 24, 24);

    h1->font_size = 32;
    h1->font_weight = 700;
    set_margin(h1, 0, 0, 16, 0);
    h1->color = CSS_RGB(0x10, 0x2A, 0x43);

    h2->font_size = 24;
    h2->font_weight = 700;
    set_margin(h2, 24, 0, 8, 0);
    h2->color = CSS_RGB(0x10, 0x2A, 0x43);

    set_margin(para, 0, 0, 14, 0);
    para->line_height.type = CSS_LEN_PX;
    para->line_height.v = 22;

    set_margin(quote, 14, 0, 14, 0);
    set_padding(quote, 8, 12, 8, 12);
    quote->border_width[CSS_LEFT] = 4;
    quote->border_style[CSS_LEFT] = CSS_BORDERSTYLE_SOLID;
    quote->border_color[CSS_LEFT] = CSS_RGB(0x5B, 0xA3, 0xD0);
    quote->background_color = CSS_RGB(0xF2, 0xF6, 0xFA);
    quote->font_style = CSS_FONTSTYLE_ITALIC;

    ul->padding[CSS_LEFT] = LPX(36);
    set_margin(ul, 8, 0, 14, 0);
    li->display = CSS_DISPLAY_LIST_ITEM;
    li->list_style_type = CSS_LISTSTYLE_DISC;
    set_margin(li, 0, 0, 4, 0);

    a->display = CSS_DISPLAY_INLINE;
    a->color = CSS_RGB(0x1A, 0x5F, 0xB4);
    a->text_decoration = CSS_DECOR_UNDERLINE;

    code->display = CSS_DISPLAY_INLINE;
    code->background_color = CSS_RGB(0xEC, 0xEF, 0xF2);
    code->font_family = CSS_FONTFAMILY_MONO;
    set_padding(code, 0, 3, 0, 3);

    fl->css_float = CSS_FLOAT_RIGHT;
    fl->width = LPX(140);
    fl->height = LPX(90);
    fl->background_color = CSS_RGB(0xD8, 0xE4, 0xEE);
    set_margin(fl, 4, 0, 8, 14);
    set_border(fl, 1, CSS_RGB(0x9F, 0xB4, 0xC6));

    tb->border_spacing = 0;
    set_margin(tb, 8, 0, 16, 0);
    tb->width = LPCT(100);
    set_border(th, 1, CSS_RGB(0xB0, 0xB8, 0xC0));
    set_padding(th, 5, 8, 5, 8);
    th->font_weight = 700;
    th->background_color = CSS_RGB(0xE8, 0xEC, 0xF0);
    set_border(td, 1, CSS_RGB(0xD0, 0xD6, 0xDC));
    set_padding(td, 5, 8, 5, 8);

    b = el(t, 0, "body", body);

    n = el(t, b, "h1", h1);
    txt(t, n, "The Kestrel layout engine");

    n = el(t, b, "div", fl);
    txt(t, n, "A floated aside, 140px wide, that the text flows around.");

    n = el(t, b, "p", para);
    txt(t, n, "This paragraph is laid out by a from-scratch implementation "
              "of the CSS 2.1 visual formatting model. Words are measured "
              "with real font metrics and broken at the last opportunity "
              "that fits, so the right edge is ragged in the way a reader "
              "expects rather than clipped mid-glyph. ");
    p2 = el(t, n, "a", a);
    txt(t, p2, "Links are underlined");
    txt(t, n, " and inline code such as ");
    p2 = el(t, n, "code", code);
    txt(t, p2, "margin-collapse");
    txt(t, n, " keeps its own background across the line break.");

    n = el(t, b, "blockquote", quote);
    txt(t, n, "Vertical margins between adjacent blocks collapse to the "
              "larger of the two. Without that rule every page looks "
              "subtly wrong, and with it the spacing above reads as "
              "deliberate.");

    n = el(t, b, "h2", h2);
    txt(t, n, "What the engine does");

    n = el(t, b, "ul", ul);
    for (i = 0; i < 4; i++) {
        static const char *const items[4] = {
            "Block and inline formatting with line boxes.",
            "Floats, with line boxes shortened around them and clear.",
            "Tables measured column by column, with colspan and rowspan.",
            "Absolute, fixed and relative positioning with a z-order."
        };
        struct dom_node *it = el(t, n, "li", li);

        txt(t, it, items[i]);
    }

    n = el(t, b, "h2", h2);
    txt(t, n, "A measured table");

    tab = el(t, b, "table", tb);
    r = el(t, tab, "tr", tr);
    {
        static const char *const head[3] = {"Feature", "Status", "Notes"};

        for (i = 0; i < 3; i++) {
            struct dom_node *cell = el(t, r, "th", th);

            txt(t, cell, head[i]);
        }
    }
    {
        static const char *const rows[3][3] = {
            {"Margin collapsing", "done", "all three forms"},
            {"Automatic tables", "done",
             "min and max content widths per column"},
            {"Floats", "done", "block level and inline"}
        };

        for (i = 0; i < 3; i++) {
            int j;

            r = el(t, tab, "tr", tr);
            for (j = 0; j < 3; j++) {
                struct dom_node *cell = el(t, r, "td", td);

                txt(t, cell, rows[i][j]);
            }
        }
    }
    return b;
}

#ifdef LAYOUT_TEST_WRAP_ALLOC
static void test_paint_order_oom(void)
{
    static const char *const allocation[4] = {
        "positioned boxes", "paint keys", "sort scratch", "paint order"
    };
    struct tctx *t = tnew();
    struct dom_node *b = build_article(t);
    struct lay_document *L;
    const struct lay_paint_item *items;
    unsigned long total, first, call, seen;
    unsigned long boxes;
    int paints;
    int i;

    GROUP("paint order late-allocation OOM");

    /* Calibrate only layout.c allocations on the exact fixture under test. */
    oom_begin(0);
    L = run(t, b, 760, 900);
    total = oom_end();
    CHECK(L != 0);
    if (!L) {
        tfree(t);
        return;
    }
    boxes = lay_box_count(L);
    items = 0;
    paints = lay_paint_order(L, &items);
    CHECK(boxes > 1);
    CHECK(paints > 0);
    CHECK(items != 0);
    CHECK_EQ(lay_truncated(L), 0);
    CHECK(total >= 4);
    CHECK(total <= OOM_TRACE_MAX);
    if (total < 4 || total > OOM_TRACE_MAX) {
        lay_free(L);
        tfree(t);
        return;
    }

    first = total - 3;
    CHECK_EQ(oom_sizes[first - 1], (boxes + 1) * sizeof(void *));
    CHECK_EQ(oom_sizes[first], oom_sizes[first + 1]);
    CHECK_EQ(oom_sizes[first + 2],
             ((unsigned long)paints + 1) *
                 sizeof(struct lay_paint_item));
    printf("  calibrated: %lu mallocs, %lu boxes, %d paint items\n",
           total, boxes, paints);
    printf("  late ordinals: %lu..%lu, sizes %lu/%lu/%lu/%lu bytes\n",
           first, total,
           (unsigned long)oom_sizes[first - 1],
           (unsigned long)oom_sizes[first],
           (unsigned long)oom_sizes[first + 1],
           (unsigned long)oom_sizes[first + 2]);
    lay_free(L);

    for (i = 0; i < 4; i++) {
        call = first + (unsigned long)i;
        items = (const struct lay_paint_item *)(uintptr_t)1;
        oom_begin(call);
        L = run(t, b, 760, 900);
        seen = oom_end();

        CHECK_EQ(oom_failures, 1);
        CHECK(seen >= call);
        CHECK(L != 0);
        if (L) {
            CHECK_EQ(lay_box_count(L), boxes);
            CHECK_EQ(lay_paint_order(L, &items), 0);
            CHECK(items == 0);
            CHECK_EQ(lay_truncated(L), LAY_TRUNC_MEMORY);
            lay_free(L);
            L = 0;
            lay_free(L);
        }
        printf("  injected %-16s at malloc %lu: %lu calls observed\n",
               allocation[i], call, seen);
    }
    tfree(t);
}
#endif

static void render_pages(void)
{
    struct tctx *t = tnew();
    struct dom_node *b;
    struct lay_document *L;
    struct lay_opts o;

    GROUP("rendering pages to PPM");

    b = build_article(t);
    lay_opts_init(&o, 760, 900);
    L = lay_layout_node(b, &o);
    CHECK(L != 0);
    CHECK_EQ(lay_truncated(L), 0);
    shot(L, "article", 760, 900);
    lay_free(L);

    /* the same document at a phone width: everything must reflow */
    lay_opts_init(&o, 360, 1100);
    L = lay_layout_node(b, &o);
    shot(L, "article-narrow", 360, 1100);
    CHECK(lay_height(L) > 0);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * 12  fuzzing
 * ================================================================== */

static uint64_t rng_state = 0x2545F4914F6CDD1DULL;

static unsigned rnd(unsigned n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return n ? (unsigned)((rng_state >> 11) % n) : 0;
}

static struct css_len rnd_len(int allow_auto)
{
    struct css_len l;

    switch (rnd(7)) {
    case 0: return allow_auto ? LAUTO() : LPX(0);
    case 1: return LPX((int32_t)rnd(400));
    case 2: return LPX(-(int32_t)rnd(80));
    case 3: return LPCT((int32_t)rnd(200));
    case 4:
        l.type = CSS_LEN_CALC;
        l.pct = (int32_t)rnd(100000);
        l.v = (int32_t)rnd(80) - 20;
        return l;
    case 5: return LPX((int32_t)rnd(100000));
    default: return LAUTO();
    }
}

static void rnd_style(struct computed_style *cs)
{
    int i;

    css_style_initial(cs);
    cs->display = (uint8_t)rnd(CSS_DISPLAY_COUNT);
    cs->position = (uint8_t)rnd(CSS_POSITION_COUNT);
    cs->css_float = (uint8_t)rnd(CSS_FLOAT_COUNT);
    cs->clear = (uint8_t)rnd(CSS_CLEAR_COUNT);
    cs->text_align = (uint8_t)rnd(CSS_TEXTALIGN_COUNT);
    cs->vertical_align = (uint8_t)rnd(CSS_VALIGN_COUNT);
    cs->white_space = (uint8_t)rnd(CSS_WHITESPACE_COUNT);
    cs->overflow = (uint8_t)rnd(CSS_OVERFLOW_COUNT);
    cs->visibility = (uint8_t)rnd(CSS_VISIBILITY_COUNT);
    cs->list_style_type = (uint8_t)rnd(CSS_LISTSTYLE_COUNT);
    cs->list_style_position = (uint8_t)rnd(CSS_LISTPOS_COUNT);
    cs->border_collapse = (uint8_t)rnd(CSS_BORDERCOLLAPSE_COUNT);
    cs->text_transform = (uint8_t)rnd(CSS_TEXTTRANSFORM_COUNT);
    cs->text_decoration = (uint8_t)rnd(16);
    cs->box_sizing = (uint8_t)rnd(CSS_BOXSIZING_COUNT);
    cs->flex_direction = (uint8_t)rnd(CSS_FLEXDIR_COUNT);
    cs->flex_wrap = (uint8_t)rnd(CSS_FLEXWRAP_COUNT);
    cs->justify_content = (uint8_t)rnd(CSS_JUSTIFY_COUNT);
    cs->align_items = (uint8_t)rnd(CSS_ALIGN_COUNT);
    cs->font_weight = (uint16_t)(100 * (1 + rnd(9)));
    cs->font_style = (uint8_t)rnd(CSS_FONTSTYLE_COUNT);
    cs->font_size = (int32_t)(1 + rnd(120));
    cs->border_spacing = (int32_t)rnd(12);
    cs->z_auto = (uint8_t)rnd(2);
    cs->z_index = (int32_t)rnd(20) - 10;
    cs->opacity = rnd(3) ? 1000 : (uint16_t)rnd(1001);
    cs->flex_grow = rnd(3) ? 0 : (int32_t)rnd(4000);
    cs->flex_shrink = rnd(3) ? 1000 : (int32_t)rnd(3000);
    cs->flex_basis = rnd_len(1);
    cs->gap = rnd(3) ? 0 : (int32_t)rnd(20);
    cs->row_gap = rnd(3) ? 0 : (int32_t)rnd(20);
    cs->column_gap = rnd(3) ? 0 : (int32_t)rnd(20);
    cs->grid_ncols = (uint8_t)(rnd(4) ? 0 : (1 + rnd(6)));
    for (i = 0; i < cs->grid_ncols; i++) {
        static const uint8_t gk[] = { CSS_GRID_AUTO, CSS_GRID_PX,
                                      CSS_GRID_PCT, CSS_GRID_FR };
        cs->grid_cols[i].kind = gk[rnd(4)];
        cs->grid_cols[i].val = (cs->grid_cols[i].kind == CSS_GRID_FR ||
                                cs->grid_cols[i].kind == CSS_GRID_PCT)
            ? (int32_t)rnd(3000) : (int32_t)rnd(120);
    }
    cs->grid_col_start = rnd(4) ? 0 : (int16_t)(1 + rnd(5));
    cs->grid_col_span = (uint8_t)(1 + rnd(3));
    cs->grid_row_start = rnd(4) ? 0 : (int16_t)(1 + rnd(4));
    cs->grid_row_span = (uint8_t)(1 + rnd(2));
    cs->width = rnd_len(1);
    cs->height = rnd_len(1);
    cs->min_width = rnd(3) ? LPX(0) : rnd_len(0);
    cs->max_width = rnd(3) ? cs->max_width : rnd_len(0);
    cs->min_height = rnd(3) ? LPX(0) : rnd_len(0);
    cs->line_height = rnd(2) ? cs->line_height : LPX((int32_t)rnd(60));
    cs->text_indent = rnd(3) ? LPX(0) : rnd_len(0);
    for (i = 0; i < 4; i++) {
        cs->margin[i] = rnd(2) ? LPX((int32_t)rnd(60) - 20) : rnd_len(1);
        cs->padding[i] = rnd(2) ? LPX((int32_t)rnd(30)) : rnd_len(0);
        cs->border_width[i] = (int32_t)rnd(8);
        cs->border_radius[i] = rnd(3) ? 0 : (int32_t)rnd(32);
        cs->border_style[i] = (uint8_t)rnd(CSS_BORDERSTYLE_COUNT);
        cs->border_color[i] = 0xFF000000u | rnd(0xFFFFFF);
        cs->offset[i] = rnd(2) ? LAUTO() : rnd_len(1);
    }
    cs->color = 0xFF000000u | rnd(0xFFFFFF);
    cs->background_color = rnd(2) ? 0 : (0xFF000000u | rnd(0xFFFFFF));
    if (rnd(5) == 0) {
        cs->has_gradient = 1;
        cs->grad_nstops = (uint8_t)(1 + rnd(CSS_GRAD_MAX_STOPS));
        cs->grad_angle_deg = (int16_t)(rnd(720) - 180);
        for (i = 0; i < cs->grad_nstops; i++) {
            cs->grad_colors[i] = 0xFF000000u | rnd(0xFFFFFF);
            cs->grad_pos[i] = (int16_t)(i * 1000 /
                (cs->grad_nstops > 1 ? cs->grad_nstops - 1 : 1));
        }
    }
    if (rnd(5) == 0) {
        cs->has_shadow = 1;
        cs->shadow_inset = (uint8_t)rnd(2);
        cs->shadow_x = (int32_t)rnd(15) - 7;
        cs->shadow_y = (int32_t)rnd(15) - 7;
        cs->shadow_blur = (int32_t)rnd(12);
        cs->shadow_spread = (int32_t)rnd(9) - 4;
        cs->shadow_color = (0x80u << 24) | rnd(0xFFFFFF);
    }
    cs->align_self = (uint8_t)rnd(CSS_ALIGNSELF_COUNT);
    cs->order = rnd(4) ? 0 : ((int32_t)rnd(7) - 3);
    cs->aspect_ratio = rnd(5) ? 0 : (int32_t)(500 + rnd(2500));
    cs->object_fit = (uint8_t)rnd(CSS_OBJECTFIT_COUNT);
    cs->outline_style = rnd(4) ? CSS_BORDERSTYLE_NONE : (uint8_t)rnd(CSS_BORDERSTYLE_COUNT);
    cs->outline_width = (int32_t)rnd(6);
    cs->outline_offset = (int32_t)rnd(11) - 5;
    cs->outline_color = 0xFF000000u | rnd(0xFFFFFF);
    cs->text_overflow = (uint8_t)rnd(CSS_TEXTOVERFLOW_COUNT);
    cs->word_break = (uint8_t)rnd(CSS_WORDBREAK_COUNT);
    cs->overflow_wrap = (uint8_t)rnd(CSS_OVERFLOWWRAP_COUNT);
    cs->letter_spacing = rnd(4) ? 0 : ((int32_t)rnd(7) - 2);
    cs->word_spacing = rnd(4) ? 0 : ((int32_t)rnd(9) - 3);
    if (rnd(6) == 0) {
        cs->has_transform = 1;
        cs->translate_x = rnd_len(1);
        cs->translate_y = rnd_len(1);
        cs->scale_x = (int32_t)(500 + rnd(2000));
        cs->scale_y = (int32_t)(500 + rnd(2000));
    }
}

static const char *const fuzz_words[] = {
    "alpha", "b", "gamma delta", "  ", "\n", "a\tb", "loooooooooooooong",
    "x y z", "", "one two three four five", "\n\n", "  spaced  out  "
};

static void fuzz_build(struct tctx *t, struct dom_node *parent, int depth)
{
    static const char *const tags[] = {"div", "span", "p", "table", "tr",
                                       "td", "li", "ul", "a", "img", "br"};
    int n = (int)rnd(5);
    int i;

    if (depth > 6)
        return;
    for (i = 0; i < n; i++) {
        struct dom_node *e;
        struct computed_style *cs;

        if (rnd(3) == 0) {
            txt(t, parent,
                fuzz_words[rnd(sizeof fuzz_words / sizeof fuzz_words[0])]);
            continue;
        }
        cs = calloc(1, sizeof *cs);
        rnd_style(cs);
        if (t->npool < POOL_MAX)
            t->pool[t->npool++] = cs;
        e = el(t, parent, tags[rnd(sizeof tags / sizeof tags[0])], cs);
        if (rnd(4) == 0)
            dom_set_attr(e, "colspan", rnd(2) ? "3" : "0");
        if (rnd(4) == 0)
            dom_set_attr(e, "rowspan", "2");
        if (rnd(4) == 0)
            dom_set_attr(e, "src", "sq.png");
        fuzz_build(t, e, depth + 1);
    }
}

static void test_fuzz(int iters)
{
    int i;
    unsigned long worst_boxes = 0;
    clock_t t0 = clock();

    GROUP("fuzz: random styles and trees under the sanitizers");

    for (i = 0; i < iters; i++) {
        struct tctx *t = tnew();
        struct computed_style *root = calloc(1, sizeof *root);
        struct dom_node *b;
        struct lay_document *L;
        struct lay_opts o;
        struct paint_target tg;

        css_style_initial(root);
        root->display = CSS_DISPLAY_BLOCK;
        t->pool[t->npool++] = root;
        b = el(t, 0, "body", root);
        fuzz_build(t, b, 0);

        lay_opts_init(&o, 1 + (int)rnd(1200), 1 + (int)rnd(900));
        o.image_size = fake_image_size;
        L = lay_layout_node(b, &o);
        if (L) {
            if (lay_box_count(L) > worst_boxes)
                worst_boxes = lay_box_count(L);
            /* hit testing must not fault anywhere on or off the page */
            lay_hit_test(L, (int32_t)rnd(2000) - 500,
                         (int32_t)rnd(4000) - 500);
            lay_node_at(L, 0, 0);
            lay_link_at(L, 10, 10, 0);
            tg = mk_target(120, 80);
            if (tg.px) {
                struct paint_opts po;

                paint_opts_init(&po);
                po.canvas = lay_canvas_color(L);
                lay_paint(L, &tg, lay_mkrect(0, 0, 120, 80), &po, 0);
                lay_paint(L, &tg, lay_mkrect(-50, -50, 400, 400), &po, 0);
                free(tg.px);
            }
            lay_free(L);
        }
        tfree(t);
    }
    printf("  %d random documents laid out and painted, worst %lu boxes, "
           "%.2f s\n", iters, worst_boxes,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    CHECK(1);
}

/* ================================================================== *
 * 13  pathological input and performance
 * ================================================================== */

static void test_pathological(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st_block(t);
    struct dom_node *n, *root;
    struct lay_document *L;
    int i;

    GROUP("pathological input degrades instead of dying");

    /* 4000 nested divs: deeper than both the box-tree cap and any
     * reasonable page */
    set_padding(cs, 1, 1, 1, 1);
    root = n = el(t, 0, "div", cs);
    for (i = 0; i < 4000; i++)
        n = el(t, n, "div", cs);
    txt(t, n, "deep");
    L = run(t, root, 800, 600);
    CHECK(L != 0);
    CHECK(lay_truncated(L) & LAY_TRUNC_DEPTH);
    CHECK(lay_box_count(L) < 4000);
    {
        struct lay_stats s;

        lay_get_stats(L, &s);
        printf("  4000-deep nesting: %lu boxes, trunc=0x%02x, "
               "stack %lu bytes\n",
               lay_box_count(L), lay_truncated(L), s.stack_bytes);
        printf("  deepest recursion used %lu bytes of stack\n",
               s.stack_bytes);
        CHECK(s.stack_bytes > 0);
        if (stack_gate_mode)
            CHECK(s.stack_bytes < 48UL * 1024UL);
    }
    lay_free(L);
    tfree(t);

    /* a single enormous word in a one-pixel-wide block */
    t = tnew();
    cs = st_block(t);
    cs->width = LPX(1);
    root = el(t, 0, "div", cs);
    {
        char *big = malloc(20001);

        memset(big, 'W', 20000);
        big[20000] = 0;
        txt(t, root, big);
        free(big);
    }
    L = run(t, root, 800, 600);
    CHECK(L != 0);
    CHECK(lay_height(L) > 0);
    printf("  20 000-char word at width 1: %lu boxes, %ld px tall\n",
           lay_box_count(L), (long)lay_height(L));
    lay_free(L);
    tfree(t);

    /* a table claiming enormous spans */
    t = tnew();
    {
        struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
        struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
        struct computed_style *td = st_disp(t, CSS_DISPLAY_TABLE_CELL);
        struct dom_node *tab = el(t, 0, "table", tb);

        for (i = 0; i < 40; i++) {
            struct dom_node *r = el(t, tab, "tr", tr);
            struct dom_node *c = el(t, r, "td", td);

            dom_set_attr(c, "colspan", "9999");
            dom_set_attr(c, "rowspan", "9999");
            txt(t, c, "x");
        }
        L = run(t, tab, 800, 600);
        CHECK(L != 0);
        printf("  colspan/rowspan 9999 x40: %lu boxes, %ld px tall\n",
               lay_box_count(L), (long)lay_height(L));
        lay_free(L);
    }
    tfree(t);
}

static void test_performance(void)
{
    struct tctx *t = tnew();
    struct computed_style *body = st_block(t), *h = st_block(t);
    struct computed_style *p = st_block(t), *a = st(t);
    struct computed_style *tb = st_disp(t, CSS_DISPLAY_TABLE);
    struct computed_style *tr = st_disp(t, CSS_DISPLAY_TABLE_ROW);
    struct computed_style *td = st_disp(t, CSS_DISPLAY_TABLE_CELL);
    struct dom_node *b;
    struct lay_document *L;
    struct lay_opts o;
    struct lay_stats s;
    clock_t t0, t1;
    int i, j;

    GROUP("performance on a large realistic page");

    body->width = LPCT(100);
    set_padding(body, 20, 20, 20, 20);
    h->font_size = 24;
    h->font_weight = 700;
    set_margin(h, 20, 0, 8, 0);
    set_margin(p, 0, 0, 12, 0);
    a->display = CSS_DISPLAY_INLINE;
    a->color = CSS_RGB(0x1A, 0x5F, 0xB4);
    a->text_decoration = CSS_DECOR_UNDERLINE;
    set_border(td, 1, CSS_RGB(0xCC, 0xCC, 0xCC));
    set_padding(td, 4, 6, 4, 6);

    b = el(t, 0, "body", body);
    for (i = 0; i < 400; i++) {
        struct dom_node *n = el(t, b, "h2", h);

        txt(t, n, "Section heading with a reasonable amount of text in it");
        for (j = 0; j < 4; j++) {
            struct dom_node *q = el(t, b, "p", p);
            struct dom_node *k;

            txt(t, q, "A paragraph of ordinary prose, long enough to wrap "
                      "over several line boxes at a typical reading width, "
                      "which is what the line breaker actually costs on a "
                      "real page rather than on a synthetic benchmark. ");
            k = el(t, q, "a", a);
            txt(t, k, "an inline link");
            txt(t, q, " followed by more running text to close the "
                      "paragraph off neatly.");
        }
        if (i % 10 == 0) {
            struct dom_node *tab = el(t, b, "table", tb);
            int r;

            for (r = 0; r < 6; r++) {
                struct dom_node *row = el(t, tab, "tr", tr);
                int c;

                for (c = 0; c < 4; c++) {
                    struct dom_node *cell = el(t, row, "td", td);

                    txt(t, cell, c == 0 ? "Identifier" : "some cell text");
                }
            }
        }
    }

    lay_opts_init(&o, 900, 1000);
    t0 = clock();
    L = lay_layout_node(b, &o);
    t1 = clock();
    CHECK(L != 0);
    lay_get_stats(L, &s);
    printf("  %lu DOM nodes -> %lu boxes (%lu text, %lu line, %lu tables)\n",
           (unsigned long)t->doc->nnodes, lay_box_count(L), s.text_boxes,
           s.line_boxes, s.tables);
    printf("  layout: %.1f ms, %.2f us per box, %ld px of document\n",
           1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC,
           1e6 * ((double)(t1 - t0) / CLOCKS_PER_SEC) /
               (double)(lay_box_count(L) ? lay_box_count(L) : 1),
           (long)lay_height(L));
    printf("  memory: %lu KiB for the box tree, stack %lu bytes, depth %lu\n",
           lay_memory_used(L) / 1024, s.stack_bytes, s.max_depth);
    CHECK_EQ(lay_truncated(L), 0);

    /* full repaint versus one scroll step */
    {
        struct paint_target tg = mk_target(900, 1000);
        struct paint_opts po;
        struct paint_stats full, strip;
        clock_t p0, p1, p2;

        paint_opts_init(&po);
        po.canvas = lay_canvas_color(L);
        p0 = clock();
        lay_paint(L, &tg, lay_mkrect(0, 0, 900, 1000), &po, &full);
        p1 = clock();
        po.scroll_y = 40;
        lay_paint(L, &tg, lay_mkrect(0, 960, 900, 40), &po, &strip);
        p2 = clock();
        printf("  paint: full screen %.1f ms (%lu of %lu items), "
               "40px strip %.2f ms (%lu items)\n",
               1000.0 * (double)(p1 - p0) / CLOCKS_PER_SEC,
               full.items_drawn, full.items_considered,
               1000.0 * (double)(p2 - p1) / CLOCKS_PER_SEC,
               strip.items_drawn);
        CHECK(strip.items_drawn * 20 < full.items_drawn);
        free(tg.px);
    }
    lay_free(L);
    tfree(t);
}

static struct css_len LCALC(int32_t pct_th, int32_t px)
{
    struct css_len l;

    l.v = px;
    l.pct = pct_th;
    l.type = CSS_LEN_CALC;
    return l;
}

static void test_box_sizing_and_calc(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *bb = st_block(t), *ov = st_block(t);
    struct computed_style *mm = st_block(t);
    struct computed_style *ib = st_disp(t, CSS_DISPLAY_INLINE_BLOCK);
    struct computed_style *fl = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *fi = st_block(t);
    struct computed_style *c1 = st_block(t), *c2 = st_block(t);
    struct computed_style *c3 = st_block(t), *c4 = st_block(t);
    struct dom_node *d, *nb, *nov, *nmm, *nib, *nfl, *nfi, *nc1, *nc2, *nc3, *nc4;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("box-sizing border-box and CSS_LEN_CALC");

    root->width = LPX(400);

    bb->box_sizing = CSS_BOXSIZING_BORDER;
    bb->width = LPX(200);
    bb->height = LPX(100);
    set_padding(bb, 10, 10, 10, 10);
    set_border(bb, 5, 0xFF000000u);

    ov->box_sizing = CSS_BOXSIZING_BORDER;
    ov->width = LPX(16);
    ov->height = LPX(12);
    set_padding(ov, 15, 15, 15, 15);
    set_border(ov, 5, 0xFF000000u);

    mm->box_sizing = CSS_BOXSIZING_BORDER;
    mm->width = LPX(200);
    mm->max_width = LPX(160);
    mm->height = LPX(30);
    set_padding(mm, 5, 10, 5, 10);

    ib->box_sizing = CSS_BOXSIZING_BORDER;
    ib->width = LPX(120);
    ib->height = LPX(40);
    set_padding(ib, 4, 10, 4, 10);
    set_border(ib, 2, 0xFF000000u);

    fl->width = LPX(300);
    fi->box_sizing = CSS_BOXSIZING_BORDER;
    fi->width = LPX(120);
    fi->height = LPX(50);
    set_padding(fi, 5, 10, 5, 10);
    set_border(fi, 5, 0xFF000000u);

    c1->width = LCALC(50000, -20);
    c1->height = LPX(10);

    c2->box_sizing = CSS_BOXSIZING_BORDER;
    c2->width = LCALC(25000, 10);
    c2->height = LPX(20);
    set_padding(c2, 0, 5, 0, 5);

    c3->width = LPX(40);
    c3->height = LCALC(50000, 10); /* indefinite % -> height falls back to content */

    c4->width = LPX(40);
    c4->height = LCALC(0, 50);     /* 0% + 50px -> definite 50px */

    d = el(t, 0, "div", root);
    nb = el(t, d, "div", bb);
    nov = el(t, d, "div", ov);
    nmm = el(t, d, "div", mm);
    nib = el(t, d, "span", ib);
    nfl = el(t, d, "div", fl);
    nfi = el(t, nfl, "div", fi);
    nc1 = el(t, d, "div", c1);
    nc2 = el(t, d, "div", c2);
    nc3 = el(t, d, "div", c3);
    nc4 = el(t, d, "div", c4);

    L = run(t, d, 500, 400);

    b = lay_box_for_node(L, nb);
    CHECK_EQ(lay_border_rect(b).w, 200);
    CHECK_EQ(lay_border_rect(b).h, 100);
    CHECK_EQ(b->w, 170);
    CHECK_EQ(b->h, 70);

    b = lay_box_for_node(L, nov);
    CHECK_EQ(b->w, 0);
    CHECK_EQ(b->h, 0);
    CHECK_EQ(lay_border_rect(b).w, 40);

    b = lay_box_for_node(L, nmm);
    CHECK_EQ(lay_border_rect(b).w, 160);
    CHECK_EQ(b->w, 140);

    b = lay_box_for_node(L, nib);
    CHECK_EQ(lay_border_rect(b).w, 120);
    CHECK_EQ(b->w, 96);

    b = lay_box_for_node(L, nfi);
    CHECK_EQ(lay_border_rect(b).w, 120);
    CHECK_EQ(b->w, 90);
    CHECK_EQ(lay_border_rect(b).h, 50);
    CHECK_EQ(b->h, 30);

    CHECK_EQ(lay_box_for_node(L, nc1)->w, 180);
    b = lay_box_for_node(L, nc2);
    CHECK_EQ(lay_border_rect(b).w, 110);
    CHECK_EQ(b->w, 100);
    CHECK_EQ(lay_box_for_node(L, nc3)->h, 0);
    CHECK_EQ(lay_box_for_node(L, nc4)->h, 50);

    lay_free(L);
    tfree(t);
}

static void test_flex_complete(void)
{
    struct tctx *t = tnew();
    struct computed_style *fw = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *s1 = st_block(t), *s2 = st_block(t), *s3 = st_block(t);
    struct computed_style *fsh = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *sh1 = st_block(t), *sh2 = st_block(t);
    struct computed_style *fbl = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *bl1 = st_block(t), *bl2 = st_block(t);
    struct computed_style *fcw = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *cw1 = st_block(t), *cw2 = st_block(t);
    struct dom_node *root, *d, *na, *nb, *nc, *nd_sh, *ns1, *ns2;
    struct dom_node *nd_bl, *nb1, *nb2, *nd_cw, *nc1, *nc2;
    struct lay_document *L;
    struct lay_box *pa, *pb, *pc, *pfw;

    GROUP("flexbox: wrap, gaps, flex-basis, flex-shrink, baseline");

    root = el(t, 0, "div", st_block(t));

    /* A: flex-wrap: wrap with column-gap, row-gap, space-between */
    fw->width = LPX(200);
    fw->flex_wrap = CSS_FLEXWRAP_WRAP;
    fw->column_gap = 10;
    fw->row_gap = 20;
    fw->justify_content = CSS_JUSTIFY_SPACE_BETWEEN;
    fw->align_items = CSS_ALIGN_START;
    s1->width = LPX(80);  s1->height = LPX(30);
    s2->width = LPX(90);  s2->height = LPX(40);
    s3->width = LPX(70);  s3->height = LPX(25);
    d = el(t, root, "div", fw);
    na = el(t, d, "div", s1);
    nb = el(t, d, "div", s2);
    nc = el(t, d, "div", s3);

    /* C: weighted flex-shrink + flex-basis */
    fsh->width = LPX(200);
    fsh->flex_wrap = CSS_FLEXWRAP_NOWRAP;
    sh1->flex_basis = LPX(150); sh1->flex_shrink = 1000; sh1->height = LPX(20);
    sh2->flex_basis = LPX(150); sh2->flex_shrink = 2000; sh2->height = LPX(20);
    nd_sh = el(t, root, "div", fsh);
    ns1 = el(t, nd_sh, "div", sh1);
    ns2 = el(t, nd_sh, "div", sh2);

    /* D: align-items: baseline across different font sizes */
    fbl->width = LPX(200);
    fbl->align_items = CSS_ALIGN_BASELINE;
    bl1->font_size = 12;
    bl2->font_size = 32;
    nd_bl = el(t, root, "div", fbl);
    nb1 = el(t, nd_bl, "div", bl1);
    txt(t, nb1, "hi");
    nb2 = el(t, nd_bl, "div", bl2);
    txt(t, nb2, "HI");

    /* E: column flex-wrap with gaps */
    fcw->width = LPX(150);
    fcw->height = LPX(100);
    fcw->flex_direction = CSS_FLEXDIR_COLUMN;
    fcw->flex_wrap = CSS_FLEXWRAP_WRAP;
    fcw->align_items = CSS_ALIGN_START;
    fcw->row_gap = 10;
    fcw->column_gap = 15;
    cw1->width = LPX(40); cw1->height = LPX(60);
    cw2->width = LPX(40); cw2->height = LPX(60);
    nd_cw = el(t, root, "div", fcw);
    nc1 = el(t, nd_cw, "div", cw1);
    nc2 = el(t, nd_cw, "div", cw2);

    L = run(t, root, 400, 400);

    pfw = lay_box_for_node(L, d);
    pa = lay_box_for_node(L, na);
    pb = lay_box_for_node(L, nb);
    pc = lay_box_for_node(L, nc);
    CHECK_EQ(pa->x, 0);
    CHECK_EQ(pa->y, 0);
    CHECK_EQ(pb->x, 110);
    CHECK_EQ(pb->y, 0);
    CHECK_EQ(pc->x, 0);
    CHECK_EQ(pc->y, 60);
    CHECK_EQ(pfw->h, 85);

    CHECK_EQ(lay_box_for_node(L, ns1)->w, 117);
    CHECK_EQ(lay_box_for_node(L, ns2)->w, 83);

    {
        struct lay_box *lb1 = lay_box_for_node(L, nb1);
        struct lay_box *lb2 = lay_box_for_node(L, nb2);

        CHECK(lb1->y > lb2->y);
    }

    CHECK_EQ(lay_box_for_node(L, nc1)->x, 0);
    CHECK_EQ(lay_box_for_node(L, nc1)->y, lay_box_for_node(L, nd_cw)->y);
    CHECK_EQ(lay_box_for_node(L, nc2)->x, 55);
    CHECK_EQ(lay_box_for_node(L, nc2)->y, lay_box_for_node(L, nc1)->y);
    lay_free(L);

    /* B: flex-wrap: wrap-reverse puts line 2 above line 1 */
    fw->flex_wrap = CSS_FLEXWRAP_WRAP_REVERSE;
    L = run(t, root, 400, 400);
    pa = lay_box_for_node(L, na);
    pb = lay_box_for_node(L, nb);
    pc = lay_box_for_node(L, nc);
    CHECK_EQ(pc->y, 0);
    CHECK_EQ(pa->y, 45);
    CHECK_EQ(pb->y, 45);

    lay_free(L);
    tfree(t);
}

static void test_grid(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *g1 = st_disp(t, CSS_DISPLAY_GRID);
    struct computed_style *c0 = st_block(t), *c1 = st_block(t);
    struct computed_style *c2 = st_block(t), *c3 = st_block(t);
    struct computed_style *g2 = st_disp(t, CSS_DISPLAY_INLINE_GRID);
    struct computed_style *a0 = st_block(t), *a1 = st_block(t);
    struct computed_style *g3 = st_disp(t, CSS_DISPLAY_GRID);
    struct computed_style *s0 = st_block(t), *s1 = st_block(t);
    struct dom_node *d, *ng1, *n0, *n1, *n2, *n3, *ng2, *na0, *na1, *ng3, *ns0, *ns1;
    struct lay_document *L;
    struct lay_box *b;

    GROUP("css grid: tracks, span, placement, auto, inline-grid");

    root->width = LPX(400);

    g1->width = LPX(320);
    g1->grid_ncols = 3;
    g1->grid_cols[0].kind = CSS_GRID_PX;  g1->grid_cols[0].val = 60;
    g1->grid_cols[1].kind = CSS_GRID_FR;  g1->grid_cols[1].val = 1000;
    g1->grid_cols[2].kind = CSS_GRID_FR;  g1->grid_cols[2].val = 2000;
    g1->column_gap = 10;
    g1->row_gap = 15;
    g1->align_items = CSS_ALIGN_START;

    c0->height = LPX(30);
    c1->grid_col_span = 2; c1->height = LPX(20);
    c2->height = LPX(50); c2->box_sizing = CSS_BOXSIZING_BORDER;
    set_padding(c2, 5, 5, 5, 5);
    c3->grid_col_start = 3; c3->height = LPX(25);

    g2->grid_ncols = 2;
    g2->grid_cols[0].kind = CSS_GRID_AUTO;
    g2->grid_cols[1].kind = CSS_GRID_AUTO;
    g2->column_gap = 10;
    g2->align_items = CSS_ALIGN_CENTER;
    a0->width = LPX(50); a0->height = LPX(40);
    a1->width = LPX(70); a1->height = LPX(20);

    g3->width = LPX(200);
    g3->grid_ncols = 0; /* defaults to 1fr */
    g3->row_gap = 10;
    s0->height = LPX(25);
    s1->height = LPX(25);

    d = el(t, 0, "div", root);
    ng1 = el(t, d, "div", g1);
    n0 = el(t, ng1, "div", c0);
    n1 = el(t, ng1, "div", c1);
    n2 = el(t, ng1, "div", c2);
    n3 = el(t, ng1, "div", c3);

    ng2 = el(t, d, "div", g2);
    na0 = el(t, ng2, "div", a0);
    na1 = el(t, ng2, "div", a1);

    ng3 = el(t, d, "div", g3);
    ns0 = el(t, ng3, "div", s0);
    ns1 = el(t, ng3, "div", s1);

    L = run(t, d, 500, 400);

    CHECK_EQ(lay_box_for_node(L, ng1)->h, 95);
    CHECK_EQ(lay_box_for_node(L, n0)->x, 0);
    CHECK_EQ(lay_box_for_node(L, n0)->w, 60);
    CHECK_EQ(lay_box_for_node(L, n1)->x, 70);
    CHECK_EQ(lay_box_for_node(L, n1)->w, 250);
    b = lay_box_for_node(L, n2);
    CHECK_EQ(b->x, 5);
    CHECK_EQ(b->y, 50);
    CHECK_EQ(b->w, 50);
    CHECK_EQ(lay_border_rect(b).w, 60);
    CHECK_EQ(lay_border_rect(b).h, 50);
    CHECK_EQ(lay_box_for_node(L, n3)->x, 160);
    CHECK_EQ(lay_box_for_node(L, n3)->w, 160);

    CHECK_EQ(lay_box_for_node(L, ng2)->w, 130);
    CHECK_EQ(lay_box_for_node(L, ng2)->h, 40);
    CHECK_EQ(lay_box_for_node(L, na0)->w, 50);
    CHECK_EQ(lay_box_for_node(L, na1)->x, 60);
    CHECK_EQ(lay_box_for_node(L, na1)->y - lay_box_for_node(L, na0)->y, 10);

    CHECK_EQ(lay_box_for_node(L, ng3)->h, 60);
    CHECK_EQ(lay_box_for_node(L, ns0)->w, 200);
    CHECK_EQ(lay_box_for_node(L, ns1)->y - lay_box_for_node(L, ns0)->y, 35);

    lay_free(L);
    tfree(t);
}

static void test_fixed_and_sticky_paint_hit(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *fix = st_block(t);
    struct computed_style *sec = st_block(t);
    struct computed_style *sp = st_block(t), *stk = st_block(t);
    struct dom_node *d, *nfix, *nsec, *nstk;
    struct lay_document *L;
    struct paint_target tg = mk_target(200, 100);
    struct paint_opts o;

    GROUP("position fixed and sticky under scroll and hit-testing");

    root->width = LPX(200);
    root->height = LPX(400);
    root->overflow = CSS_OVERFLOW_HIDDEN;
    root->background_color = CSS_RGB(0xFF, 0xFF, 0xFF);

    fix->position = CSS_POSITION_FIXED;
    fix->offset[CSS_LEFT] = LPX(10);
    fix->offset[CSS_TOP] = LPX(10);
    fix->width = LPX(40);
    fix->height = LPX(20);
    fix->background_color = CSS_RGB(0xFF, 0x00, 0x00);

    sec->width = LPX(100);
    sec->height = LPX(200);
    set_margin(sec, 20, 0, 0, 70);

    sp->height = LPX(40);

    stk->position = CSS_POSITION_STICKY;
    stk->offset[CSS_TOP] = LPX(5);
    stk->width = LPX(50);
    stk->height = LPX(20);
    stk->background_color = CSS_RGB(0x00, 0xFF, 0x00);

    d = el(t, 0, "div", root);
    nfix = el(t, d, "div", fix);
    nsec = el(t, d, "div", sec);
    (void)el(t, nsec, "div", sp);
    nstk = el(t, nsec, "div", stk);

    L = run(t, d, 200, 100);
    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);

    o.scroll_y = 0;
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 100), &o, 0);
    CHECK_EQ(px_at(&tg, 20, 20), 0xFF0000);
    CHECK_EQ(px_at(&tg, 90, 70), 0x00FF00);
    CHECK_EQ(px_at(&tg, 90, 10), 0xFFFFFF);

    /* Scrolled: fixed stays anchored, sticky sticks at viewport y = 5..25 */
    o.scroll_y = 100;
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 100), &o, 0);
    CHECK_EQ(px_at(&tg, 20, 20), 0xFF0000);
    CHECK_EQ(px_at(&tg, 90, 10), 0x00FF00);
    CHECK(lay_hit_test_view(L, 20, 20, 0, 100) == lay_box_for_node(L, nfix));
    CHECK(lay_hit_test_view(L, 90, 10, 0, 100) == lay_box_for_node(L, nstk));

    /* Scrolled past bottom of containing section (220 - 20 = 200): clamps */
    o.scroll_y = 210;
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 100), &o, 0);
    CHECK_EQ(px_at(&tg, 20, 20), 0xFF0000);
    CHECK_EQ(px_at(&tg, 90, 5), 0x00FF00);
    CHECK_EQ(px_at(&tg, 90, 15), 0xFFFFFF);

    /* Section completely off-screen: sticky gone, fixed still visible */
    o.scroll_y = 250;
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 100), &o, 0);
    CHECK_EQ(px_at(&tg, 20, 20), 0xFF0000);
    CHECK_EQ(px_at(&tg, 90, 5), 0xFFFFFF);

    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_modern_effects(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *rr = st_block(t), *rrc = st_block(t);
    struct computed_style *gr = st_block(t), *sh = st_block(t);
    struct computed_style *op = st_block(t);
    struct dom_node *d, *nrr, *nsh;
    struct lay_document *L;
    struct paint_target tg = mk_target(120, 120);
    struct paint_opts o;
    uint32_t px;

    GROUP("paint: AA border-radius, linear-gradient, box-shadow, opacity");

    root->width = LPX(120);
    root->height = LPX(120);
    root->position = CSS_POSITION_RELATIVE;

    /* 1. Rounded box + overflow: hidden clips corner of blue child */
    rr->position = CSS_POSITION_ABSOLUTE;
    rr->offset[CSS_LEFT] = LPX(10); rr->offset[CSS_TOP] = LPX(10);
    rr->width = LPX(40); rr->height = LPX(40);
    rr->border_radius[0] = rr->border_radius[1] =
        rr->border_radius[2] = rr->border_radius[3] = 12;
    rr->overflow = CSS_OVERFLOW_HIDDEN;
    rr->background_color = CSS_RGB(0xFF, 0x00, 0x00);
    rrc->width = LPX(40); rrc->height = LPX(40);
    rrc->background_color = CSS_RGB(0x00, 0x00, 0xFF);

    /* 2. Linear gradient 90deg (left blue -> right red) */
    gr->position = CSS_POSITION_ABSOLUTE;
    gr->offset[CSS_LEFT] = LPX(60); gr->offset[CSS_TOP] = LPX(10);
    gr->width = LPX(40); gr->height = LPX(40);
    gr->has_gradient = 1;
    gr->grad_nstops = 2;
    gr->grad_angle_deg = 90;
    gr->grad_colors[0] = CSS_RGB(0x00, 0x00, 0xFF);
    gr->grad_pos[0] = 0;
    gr->grad_colors[1] = CSS_RGB(0xFF, 0x00, 0x00);
    gr->grad_pos[1] = 1000;

    /* 3. Drop shadow */
    sh->position = CSS_POSITION_ABSOLUTE;
    sh->offset[CSS_LEFT] = LPX(20); sh->offset[CSS_TOP] = LPX(70);
    sh->width = LPX(30); sh->height = LPX(20);
    sh->background_color = CSS_RGB(0xFF, 0xFF, 0xFF);
    sh->has_shadow = 1;
    sh->shadow_x = 6;
    sh->shadow_y = 6;
    sh->shadow_blur = 6;
    sh->shadow_color = CSS_RGB(0x00, 0x00, 0x00);

    /* 4. Opacity 50% */
    op->position = CSS_POSITION_ABSOLUTE;
    op->offset[CSS_LEFT] = LPX(70); op->offset[CSS_TOP] = LPX(70);
    op->width = LPX(30); op->height = LPX(20);
    op->opacity = 500;
    op->background_color = CSS_RGB(0x00, 0x00, 0x00);

    d = el(t, 0, "div", root);
    nrr = el(t, d, "div", rr);
    (void)el(t, nrr, "div", rrc);
    (void)el(t, d, "div", gr);
    nsh = el(t, d, "div", sh);
    (void)el(t, d, "div", op);

    L = run(t, d, 120, 120);
    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &tg, lay_mkrect(0, 0, 120, 120), &o, 0);

    CHECK_EQ(px_at(&tg, 10, 10), 0xFFFFFF);
    CHECK_EQ(px_at(&tg, 30, 30), 0x0000FF);

    px = px_at(&tg, 62, 30);
    CHECK((px & 0xFF) > 200 && ((px >> 16) & 0xFF) < 50);
    px = px_at(&tg, 98, 30);
    CHECK(((px >> 16) & 0xFF) > 200 && (px & 0xFF) < 50);

    CHECK(lay_paint_rect(lay_box_for_node(L, nsh)).w >
          lay_border_rect(lay_box_for_node(L, nsh)).w);
    CHECK(px_at(&tg, 40, 92) < 0xFFFFFFu);
    CHECK_EQ(px_at(&tg, 5, 70), 0xFFFFFF);

    px = px_at(&tg, 85, 80);
    CHECK((px & 0xFF) >= 110 && (px & 0xFF) <= 145);

    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_paint_svg_modern(void)
{
    struct tctx *t = tnew();
    struct computed_style *cs = st_block(t);
    struct dom_node *svg, *hole, *smooth, *quad, *svg_none, *nr;
    struct lay_document *L;
    struct paint_target tg;
    struct paint_opts o;

    GROUP("paint: modern SVG subpaths, S/T beziers, currentColor, aspect");

    cs->color = CSS_RGB(0x00, 0xAA, 0x44);

    svg = el(t, 0, "svg", cs);
    dom_set_attr(svg, "width", "100");
    dom_set_attr(svg, "height", "80");

    /* Compound even-odd square with center cutout filled via currentColor */
    hole = el(t, svg, "path", cs);
    dom_set_attr(hole, "d",
                 "M 10 10 L 50 10 L 50 50 L 10 50 Z "
                 "M 20 20 L 20 40 L 40 40 L 40 20 Z");
    dom_set_attr(hole, "fill", "currentColor");

    /* Smooth cubic S curve stroked in currentColor */
    smooth = el(t, svg, "path", cs);
    dom_set_attr(smooth, "d", "M 60 20 C 70 5 80 5 82 25 S 95 40 95 20");
    dom_set_attr(smooth, "fill", "none");
    dom_set_attr(smooth, "stroke", "currentColor");
    dom_set_attr(smooth, "stroke-width", "2");

    /* Smooth quadratic T curve */
    quad = el(t, svg, "path", cs);
    dom_set_attr(quad, "d", "M 60 60 Q 70 45 80 60 T 95 60");
    dom_set_attr(quad, "fill", "none");
    dom_set_attr(quad, "stroke", "#FF0000");
    dom_set_attr(quad, "stroke-width", "2");

    L = run(t, svg, 100, 80);
    tg = mk_target(100, 80);
    paint_opts_init(&o);
    o.canvas = CSS_RGB(0xFF, 0xFF, 0xFF);
    lay_paint(L, &tg, lay_mkrect(0, 0, 100, 80), &o, 0);

    CHECK_EQ(px_at(&tg, 15, 15), 0x00AA44);
    CHECK_EQ(px_at(&tg, 30, 30), 0xFFFFFF);
    CHECK_EQ(px_at(&tg, 95, 20), 0x00AA44);
    CHECK(count_color(&tg, 0xFF0000) > 10);

    free(tg.px);
    lay_free(L);

    /* preserveAspectRatio="none" stretches 200x100 viewBox across 100x100 */
    svg_none = el(t, 0, "svg", cs);
    dom_set_attr(svg_none, "width", "100");
    dom_set_attr(svg_none, "height", "100");
    dom_set_attr(svg_none, "viewBox", "0 0 200 100");
    dom_set_attr(svg_none, "preserveAspectRatio", "none");
    nr = el(t, svg_none, "rect", cs);
    dom_set_attr(nr, "width", "200");
    dom_set_attr(nr, "height", "100");
    dom_set_attr(nr, "fill", "#FF0000");

    L = run(t, svg_none, 100, 100);
    tg = mk_target(100, 100);
    lay_paint(L, &tg, lay_mkrect(0, 0, 100, 100), &o, 0);
    CHECK_EQ(px_at(&tg, 50, 10), 0xFF0000);
    CHECK_EQ(px_at(&tg, 50, 90), 0xFF0000);

    free(tg.px);
    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * Wave 7 modern CSS3 layout, alignment, sizing & paint tests
 * ================================================================== */

static void test_aspect_ratio_layout(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *ar_wh = st_block(t);
    struct computed_style *ar_hw = st_block(t);
    struct computed_style *ar_bb = st_block(t);
    struct computed_style *ar_cl = st_block(t);
    struct computed_style *ar_im = st(t);
    struct dom_node *d, *n1, *n2, *n3, *n4, *ni;
    struct lay_document *L;
    struct lay_opts o;
    struct lay_box *b;

    GROUP("aspect-ratio: sizing, box-sizing, min/max, replaced");

    root->width = LPX(500);

    ar_wh->width = LPX(160);
    ar_wh->aspect_ratio = 1778; /* 16 / 9 */

    ar_hw->width = LAUTO();
    ar_hw->height = LPX(90);
    ar_hw->aspect_ratio = 1778;

    ar_bb->box_sizing = CSS_BOXSIZING_BORDER;
    ar_bb->width = LPX(100);
    set_padding(ar_bb, 10, 10, 10, 10);
    ar_bb->aspect_ratio = 2000; /* 2 / 1 -> outer 100x50 -> inner 80x30 */

    ar_cl->width = LPX(100);
    ar_cl->max_height = LPX(60);
    ar_cl->aspect_ratio = 1000; /* 1 / 1 clamped down to 60 */

    ar_im->display = CSS_DISPLAY_INLINE;
    ar_im->width = LPX(80);
    ar_im->aspect_ratio = 1000; /* 1 / 1 overrides a.png 100x50 ratio */

    d = el(t, 0, "div", root);
    n1 = el(t, d, "div", ar_wh);
    n2 = el(t, d, "div", ar_hw);
    n3 = el(t, d, "div", ar_bb);
    n4 = el(t, d, "div", ar_cl);
    ni = el(t, d, "img", ar_im);
    dom_set_attr(ni, "src", "a.png");

    lay_opts_init(&o, 500, 400);
    o.image_size = fake_image_size;
    L = lay_layout_node(d, &o);

    CHECK_EQ(lay_box_for_node(L, n1)->w, 160);
    CHECK_EQ(lay_box_for_node(L, n1)->h, 90);
    CHECK_EQ(lay_box_for_node(L, n2)->w, 160);
    CHECK_EQ(lay_box_for_node(L, n2)->h, 90);
    b = lay_box_for_node(L, n3);
    CHECK_EQ(b->w, 80);
    CHECK_EQ(b->h, 30);
    CHECK_EQ(lay_border_rect(b).h, 50);
    CHECK_EQ(lay_box_for_node(L, n4)->h, 60);
    CHECK_EQ(lay_box_for_node(L, ni)->w, 80);
    CHECK_EQ(lay_box_for_node(L, ni)->h, 80);

    lay_free(L);

    ar_cl->max_height.type = CSS_LEN_NONE;
    ar_cl->min_height = LPX(130);
    L = lay_layout_node(d, &o);
    CHECK_EQ(lay_box_for_node(L, n4)->h, 130);

    lay_free(L);
    tfree(t);
}

struct objfit_ctx {
    struct image split;
    struct image small_red;
    struct image wide_red;
};

static int objfit_size(void *ctx, const char *url, int32_t *w, int32_t *h)
{
    struct objfit_ctx *oc = (struct objfit_ctx *)ctx;
    if (!strcmp(url, "split.png")) { *w = oc->split.w; *h = oc->split.h; return 1; }
    if (!strcmp(url, "small.png")) { *w = oc->small_red.w; *h = oc->small_red.h; return 1; }
    if (!strcmp(url, "wide.png"))  { *w = oc->wide_red.w;  *h = oc->wide_red.h;  return 1; }
    return 0;
}

static const struct image *objfit_get(void *ctx, const char *url)
{
    struct objfit_ctx *oc = (struct objfit_ctx *)ctx;
    if (!strcmp(url, "split.png")) return &oc->split;
    if (!strcmp(url, "small.png")) return &oc->small_red;
    if (!strcmp(url, "wide.png"))  return &oc->wide_red;
    return 0;
}

static void init_solid_img(struct image *im, int w, int h, uint32_t px)
{
    int i;
    im->w = w;
    im->h = h;
    im->has_alpha = 0;
    im->alpha = 0;
    im->px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    for (i = 0; i < w * h; i++)
        im->px[i] = px;
}

static void test_object_fit_paint(void)
{
    struct tctx *t = tnew();
    struct objfit_ctx oc;
    struct computed_style *root = st_block(t);
    struct computed_style *s1 = st(t), *s2 = st(t), *s3 = st(t), *s4 = st(t), *s5 = st(t);
    struct dom_node *d, *i1, *i2, *i3, *i4, *i5;
    struct lay_document *L;
    struct lay_opts lo;
    struct paint_opts po;
    struct paint_target tg = mk_target(200, 120);
    int x, y;

    GROUP("object-fit: contain, cover, none, scale-down");

    init_solid_img(&oc.split, 40, 20, 0xFFFF0000u);
    for (y = 0; y < 20; y++)
        for (x = 0; x < 20; x++)
            oc.split.px[y * 40 + x] = 0xFF0000FFu; /* left blue, right red */
    init_solid_img(&oc.small_red, 10, 10, 0xFFFF0000u);
    init_solid_img(&oc.wide_red, 100, 50, 0xFFFF0000u);

    root->width = LPX(200);
    s1->display = CSS_DISPLAY_INLINE_BLOCK;
    s1->width = LPX(40); s1->height = LPX(40);
    s1->background_color = CSS_RGB(0, 255, 0);
    s1->object_fit = CSS_OBJECTFIT_CONTAIN;

    s2->display = CSS_DISPLAY_INLINE_BLOCK;
    s2->width = LPX(20); s2->height = LPX(20);
    s2->margin[CSS_LEFT] = LPX(10); s2->margin[CSS_RIGHT] = LPX(10);
    s2->background_color = CSS_RGB(0, 255, 0);
    s2->object_fit = CSS_OBJECTFIT_COVER;

    s3->display = CSS_DISPLAY_INLINE_BLOCK;
    s3->width = LPX(40); s3->height = LPX(40);
    s3->background_color = CSS_RGB(0, 255, 0);
    s3->object_fit = CSS_OBJECTFIT_NONE;

    s4->display = CSS_DISPLAY_INLINE_BLOCK;
    s4->width = LPX(40); s4->height = LPX(40);
    s4->background_color = CSS_RGB(0, 255, 0);
    s4->object_fit = CSS_OBJECTFIT_SCALE_DOWN;

    s5->display = CSS_DISPLAY_INLINE_BLOCK;
    s5->width = LPX(40); s5->height = LPX(40);
    s5->background_color = CSS_RGB(0, 255, 0);
    s5->object_fit = CSS_OBJECTFIT_SCALE_DOWN;

    d = el(t, 0, "div", root);
    i1 = el(t, d, "img", s1); dom_set_attr(i1, "src", "wide.png");
    i2 = el(t, d, "img", s2); dom_set_attr(i2, "src", "split.png");
    i3 = el(t, d, "img", s3); dom_set_attr(i3, "src", "small.png");
    i4 = el(t, d, "img", s4); dom_set_attr(i4, "src", "small.png");
    i5 = el(t, d, "img", s5); dom_set_attr(i5, "src", "wide.png");

    lay_opts_init(&lo, 200, 120);
    lo.image_size = objfit_size;
    lo.image_ctx = &oc;
    L = lay_layout_node(d, &lo);

    paint_opts_init(&po);
    po.canvas = CSS_RGB(255, 255, 255);
    po.image = objfit_get;
    po.image_ctx = &oc;
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 120), &po, 0);

    {
        struct lay_box *b1 = lay_box_for_node(L, i1);
        struct lay_box *b2 = lay_box_for_node(L, i2);
        struct lay_box *b3 = lay_box_for_node(L, i3);
        struct lay_box *b4 = lay_box_for_node(L, i4);
        struct lay_box *b5 = lay_box_for_node(L, i5);

        /* i1 contain 100x50 in 40x40 -> 40x20 at dy=10..30: top/bottom green, mid red */
        CHECK_EQ(px_at(&tg, b1->x + 20, b1->y + 5), 0x00FF00);
        CHECK_EQ(px_at(&tg, b1->x + 20, b1->y + 20), 0xFF0000);
        CHECK_EQ(px_at(&tg, b1->x + 20, b1->y + 35), 0x00FF00);

        /* i2 cover split 40x20 in 20x20: cropped horizontally -> left blue, right red */
        CHECK_EQ(px_at(&tg, b2->x + 5, b2->y + 10), 0x0000FF);
        CHECK_EQ(px_at(&tg, b2->x + 15, b2->y + 10), 0xFF0000);
        CHECK_EQ(px_at(&tg, b2->x - 2, b2->y + 10), 0xFFFFFF);
        CHECK_EQ(px_at(&tg, b2->x + 22, b2->y + 10), 0xFFFFFF);

        /* i3 none 10x10 in 40x40 -> centered 15..25 */
        CHECK_EQ(px_at(&tg, b3->x + 5, b3->y + 5), 0x00FF00);
        CHECK_EQ(px_at(&tg, b3->x + 20, b3->y + 20), 0xFF0000);

        /* i4 scale-down 10x10 in 40x40 -> picks none (10x10) */
        CHECK_EQ(px_at(&tg, b4->x + 5, b4->y + 5), 0x00FF00);
        CHECK_EQ(px_at(&tg, b4->x + 20, b4->y + 20), 0xFF0000);

        /* i5 scale-down 100x50 in 40x40 -> picks contain (40x20) */
        CHECK_EQ(px_at(&tg, b5->x + 20, b5->y + 5), 0x00FF00);
        CHECK_EQ(px_at(&tg, b5->x + 20, b5->y + 20), 0xFF0000);
    }

    free(tg.px);
    free(oc.split.px);
    free(oc.small_red.px);
    free(oc.wide_red.px);
    lay_free(L);
    tfree(t);
}

static void test_flex_grid_modern(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *fr = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *fa = st_block(t), *fb = st_block(t);
    struct computed_style *fc = st_block(t), *fd = st_block(t);
    struct computed_style *fcol = st_disp(t, CSS_DISPLAY_FLEX);
    struct computed_style *ca = st_block(t), *cb = st_block(t);
    struct computed_style *gr = st_disp(t, CSS_DISPLAY_GRID);
    struct computed_style *g0 = st_block(t), *g1 = st_block(t);
    struct computed_style *g2 = st_block(t), *g3 = st_block(t);
    struct dom_node *d, *nf, *nfa, *nfb, *nfc, *nfd;
    struct dom_node *nfc2, *nca, *ncb, *ng, *ng0, *ng1, *ng2, *ng3;
    struct lay_document *L;

    GROUP("flex & grid: order, align-self, grid-template-rows");

    root->width = LPX(300);

    fr->width = LPX(200);
    fr->height = LPX(40);
    fr->align_items = CSS_ALIGN_START;
    fa->width = LPX(40); fa->height = LPX(20); fa->order = 2;
    fb->width = LPX(40); fb->height = LPX(20); fb->order = -1;
    fb->align_self = CSS_ALIGNSELF_CENTER;
    fc->width = LPX(40); fc->height = LPX(20); fc->order = 0;
    fc->align_self = CSS_ALIGNSELF_END;
    fd->width = LPX(40); fd->height = LPX(20); fd->order = 0;

    fcol->flex_direction = CSS_FLEXDIR_COLUMN;
    fcol->width = LPX(100);
    fcol->align_items = CSS_ALIGN_START;
    ca->width = LPX(30); ca->height = LPX(20); ca->order = 1;
    ca->align_self = CSS_ALIGNSELF_END;
    cb->width = LPX(40); cb->height = LPX(20); cb->order = -2;
    cb->align_self = CSS_ALIGNSELF_CENTER;

    gr->width = LPX(200);
    gr->height = LPX(230);
    gr->grid_ncols = 2;
    gr->grid_cols[0].kind = CSS_GRID_FR; gr->grid_cols[0].val = 1000;
    gr->grid_cols[1].kind = CSS_GRID_FR; gr->grid_cols[1].val = 1000;
    gr->grid_nrows = 3;
    gr->grid_rows[0].kind = CSS_GRID_PX; gr->grid_rows[0].val = 30;
    gr->grid_rows[1].kind = CSS_GRID_FR; gr->grid_rows[1].val = 1000;
    gr->grid_rows[2].kind = CSS_GRID_FR; gr->grid_rows[2].val = 2000;
    gr->row_gap = 10;
    gr->align_items = CSS_ALIGN_START;

    g0->height = LPX(20); g0->order = 1;
    g1->height = LPX(20); g1->order = 0;
    g2->height = LPX(20); g2->order = 2; g2->grid_col_span = 2; g2->align_self = CSS_ALIGNSELF_CENTER;
    g3->height = LAUTO(); g3->order = 3; g3->align_self = CSS_ALIGNSELF_STRETCH;

    d = el(t, 0, "div", root);
    nf = el(t, d, "div", fr);
    nfa = el(t, nf, "div", fa);
    nfb = el(t, nf, "div", fb);
    nfc = el(t, nf, "div", fc);
    nfd = el(t, nf, "div", fd);

    nfc2 = el(t, d, "div", fcol);
    nca = el(t, nfc2, "div", ca);
    ncb = el(t, nfc2, "div", cb);

    ng = el(t, d, "div", gr);
    ng0 = el(t, ng, "div", g0);
    ng1 = el(t, ng, "div", g1);
    ng2 = el(t, ng, "div", g2);
    ng3 = el(t, ng, "div", g3);

    L = run(t, d, 300, 500);

    /* flex row order: fb(-1)=0, fc(0)=40, fd(0)=80, fa(2)=120 */
    CHECK_EQ(lay_box_for_node(L, nfb)->x, 0);
    CHECK_EQ(lay_box_for_node(L, nfc)->x, 40);
    CHECK_EQ(lay_box_for_node(L, nfd)->x, 80);
    CHECK_EQ(lay_box_for_node(L, nfa)->x, 120);
    /* align-self in row */
    CHECK_EQ(lay_box_for_node(L, nfb)->y, 10);
    CHECK_EQ(lay_box_for_node(L, nfc)->y, 20);
    CHECK_EQ(lay_box_for_node(L, nfd)->y, 0);

    /* flex column order & align-self */
    CHECK(lay_box_for_node(L, ncb)->y < lay_box_for_node(L, nca)->y);
    CHECK_EQ(lay_box_for_node(L, nca)->x, 70); /* 100 - 30 */
    CHECK_EQ(lay_box_for_node(L, ncb)->x, 30); /* (100 - 40) / 2 */

    /* grid order: g1(0) col 0, g0(1) col 1 */
    CHECK_EQ(lay_box_for_node(L, ng1)->x, 0);
    CHECK_EQ(lay_box_for_node(L, ng0)->x, 100);
    /* grid-template-rows: 30px (y=0..30), gap 10, 1fr=60 (y=40..100), gap 10, 2fr=120 (y=110..230) */
    {
        struct lay_box *bg = lay_box_for_node(L, ng);
        struct lay_box *bg2 = lay_box_for_node(L, ng2);
        struct lay_box *bg3 = lay_box_for_node(L, ng3);

        CHECK_EQ(bg2->y - bg->y, 40 + (60 - 20) / 2); /* centered in 60px row 1 */
        CHECK_EQ(bg3->y - bg->y, 110);
        CHECK_EQ(bg3->h, 120); /* stretched into 2fr row 2 */
    }

    lay_free(L);
    tfree(t);
}

static void test_outline_paint(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *box = st_block(t);
    struct computed_style *neg = st_block(t);
    struct dom_node *d, *nb, *nn;
    struct lay_document *L;
    struct paint_opts po;
    struct paint_target tg = mk_target(160, 160);
    lay_rect pr;

    GROUP("outline: layout invariant, offset positive/negative, overflow clip");

    root->width = LPX(160);
    root->overflow = CSS_OVERFLOW_HIDDEN;
    box->width = LPX(40);
    box->height = LPX(40);
    box->margin[CSS_TOP] = LPX(20);
    box->margin[CSS_LEFT] = LPX(20);
    box->overflow = CSS_OVERFLOW_HIDDEN;
    box->background_color = CSS_RGB(0, 255, 0);
    box->outline_width = 4;
    box->outline_style = CSS_BORDERSTYLE_SOLID;
    box->outline_color = CSS_RGB(255, 0, 0);
    box->outline_offset = 3;

    neg->width = LPX(40);
    neg->height = LPX(40);
    neg->margin[CSS_TOP] = LPX(20);
    neg->margin[CSS_LEFT] = LPX(20);
    neg->background_color = CSS_RGB(255, 255, 255);
    neg->outline_width = 2;
    neg->outline_style = CSS_BORDERSTYLE_SOLID;
    neg->outline_color = CSS_RGB(0, 0, 255);
    neg->outline_offset = -5;

    d = el(t, 0, "div", root);
    nb = el(t, d, "div", box);
    nn = el(t, d, "div", neg);

    L = run(t, d, 160, 160);
    pr = lay_paint_rect(lay_box_for_node(L, nb));
    CHECK_EQ(lay_box_for_node(L, nb)->x, 20);
    CHECK_EQ(lay_box_for_node(L, nb)->w, 40);
    CHECK_EQ(pr.x, 13);
    CHECK_EQ(pr.w, 54);

    paint_opts_init(&po);
    po.canvas = CSS_RGB(255, 255, 255);
    lay_paint(L, &tg, lay_mkrect(0, 0, 160, 160), &po, 0);

    /* outline ring at (13..16), offset gap at (17..19), box at (20..59) */
    CHECK_EQ(px_at(&tg, 15, 40), 0xFF0000);
    CHECK_EQ(px_at(&tg, 18, 40), 0xFFFFFF);
    CHECK_EQ(px_at(&tg, 40, 40), 0x00FF00);

    /* negative outline_offset (-5px, width 2px) occupies [bn->x+3 .. bn->x+4] */
    {
        struct lay_box *bn = lay_box_for_node(L, nn);
        CHECK_EQ(px_at(&tg, bn->x + 4, bn->y + 20), 0x0000FF);
        CHECK_EQ(px_at(&tg, bn->x + 5, bn->y + 20), 0xFFFFFF);
    }

    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_text_modern_layout(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *ib_ls = st(t);
    struct computed_style *wb_norm = st_block(t);
    struct computed_style *wb_all = st_block(t);
    struct computed_style *el_blk = st_block(t);
    struct computed_style *el_ib = st(t);
    struct computed_style *el_fit = st_block(t);
    struct dom_node *d, *nls, *nwn, *nwa, *nel, *neib, *nefit;
    struct lay_document *L;
    struct lay_box *b;
    struct paint_opts po;
    struct paint_target tg = mk_target(200, 260);
    int x, y, ink_in_ell = 0;

    GROUP("text polish: letter-spacing, word-break, ellipsis");

    root->width = LPX(200);

    ib_ls->display = CSS_DISPLAY_INLINE_BLOCK;
    ib_ls->letter_spacing = 4;
    ib_ls->color = CSS_RGB(0, 0, 0);

    wb_norm->width = LPX(80);
    wb_norm->overflow_wrap = CSS_OVERFLOWWRAP_BREAK_WORD;

    wb_all->width = LPX(80);
    wb_all->word_break = CSS_WORDBREAK_BREAK_ALL;

    el_blk->width = LPX(80);
    el_blk->overflow = CSS_OVERFLOW_HIDDEN;
    el_blk->white_space = CSS_WHITESPACE_NOWRAP;
    el_blk->text_overflow = CSS_TEXTOVERFLOW_ELLIPSIS;

    el_ib->display = CSS_DISPLAY_INLINE_BLOCK;
    el_ib->width = LPX(64);
    el_ib->overflow = CSS_OVERFLOW_HIDDEN;
    el_ib->white_space = CSS_WHITESPACE_NOWRAP;
    el_ib->text_overflow = CSS_TEXTOVERFLOW_ELLIPSIS;

    el_fit->width = LPX(80);
    el_fit->overflow = CSS_OVERFLOW_HIDDEN;
    el_fit->white_space = CSS_WHITESPACE_NOWRAP;
    el_fit->text_overflow = CSS_TEXTOVERFLOW_ELLIPSIS;

    d = el(t, 0, "div", root);
    nls = el(t, d, "span", ib_ls);
    txt(t, nls, "abcd");
    nwn = el(t, d, "div", wb_norm);
    txt(t, nwn, "hi superlongword");
    nwa = el(t, d, "div", wb_all);
    txt(t, nwa, "hi superlongword");
    nel = el(t, d, "div", el_blk);
    txt(t, nel, "HelloWorldExtra");
    neib = el(t, d, "span", el_ib);
    txt(t, neib, "ABCDEFGHIJKLM");
    nefit = el(t, d, "div", el_fit);
    txt(t, nefit, "HelloWorld");

    L = run(t, d, 200, 260);

    /* letter-spacing=4, FONT_BODY w=8 -> step=12, "abcd" = 3*12 + 8 = 44 */
    CHECK_EQ(lay_box_for_node(L, nls)->w, 44);

    /* overflow-wrap wraps whole word onto line 2; break-all fills line 1 */
    b = lay_box_for_node(L, nwn);
    CHECK_EQ(first_text(first_line(b))->text_len, 2);
    b = lay_box_for_node(L, nwa);
    CHECK_EQ(first_text(first_line(b))->text_len, 10);

    /* text-overflow: ellipsis produces "..." fragment inside 80px and 64px */
    {
        struct lay_box *l = first_line(lay_box_for_node(L, nel));
        struct lay_box *last = l ? l->last_child : 0;
        CHECK_EQ(l->w, 80);
        CHECK(last && last->text_len == 3 && !strncmp(last->text, "...", 3));
        CHECK_EQ(l->first_child->text_len, 7);
    }
    {
        struct lay_box *l = first_line(lay_box_for_node(L, neib));
        struct lay_box *last = l ? l->last_child : 0;
        CHECK_EQ(l->w, 64);
        CHECK(last && last->text_len == 3 && !strncmp(last->text, "...", 3));
        CHECK_EQ(l->first_child->text_len, 5);
    }
    {
        struct lay_box *l = first_line(lay_box_for_node(L, nefit));
        CHECK_EQ(l->w, 80);
        CHECK_EQ(l->first_child->text_len, 10);
        CHECK(l->first_child == l->last_child);
    }

    paint_opts_init(&po);
    po.canvas = CSS_RGB(255, 255, 255);
    lay_paint(L, &tg, lay_mkrect(0, 0, 200, 260), &po, 0);

    /* Check pixel spacing between char 0 [0..7] and char 1 [12..19] */
    {
        struct lay_box *bl = lay_box_for_node(L, nls);
        int gap_clean = 1, c1_ink = 0;
        for (y = bl->y; y < bl->y + bl->h; y++) {
            if (px_at(&tg, bl->x + 9, y) != 0xFFFFFF) gap_clean = 0;
            for (x = bl->x + 12; x < bl->x + 20; x++)
                if (px_at(&tg, x, y) != 0xFFFFFF) c1_ink = 1;
        }
        CHECK(gap_clean);
        CHECK(c1_ink);
    }
    {
        struct lay_box *bl = lay_box_for_node(L, nel);
        for (y = bl->y; y < bl->y + bl->h; y++)
            for (x = bl->x + 56; x < bl->x + 80; x++)
                if (px_at(&tg, x, y) != 0xFFFFFF)
                    ink_in_ell = 1;
        CHECK(ink_in_ell);
    }

    free(tg.px);
    lay_free(L);
    tfree(t);
}

static void test_word_spacing(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *norm = st_block(t);
    struct computed_style *wide = st_block(t);
    struct computed_style *pre = st_block(t);
    struct dom_node *d, *nn, *nw, *np;
    struct lay_document *L;
    struct lay_box *l, *tb;

    GROUP("inline: word-spacing widens word separators");

    root->width = LPX(200);
    wide->word_spacing = 6;
    pre->white_space = CSS_WHITESPACE_PRE;
    pre->word_spacing = 4;

    d = el(t, 0, "div", root);
    nn = el(t, d, "div", norm);
    txt(t, nn, "ab cd");
    nw = el(t, d, "div", wide);
    txt(t, nw, "ab cd");
    np = el(t, d, "div", pre);
    txt(t, np, "a  b");

    L = run(t, d, 200, 200);

    /* FONT_BODY w=8: "ab cd" is 5 glyph cells = 40px. */
    l = first_line(lay_box_for_node(L, nn));
    CHECK_EQ(l->w, 8 * 5);

    /* word-spacing 6: the one collapsed space is 8+6 wide. Frags
     * coalesce, so the whole run is one text box of 46px. */
    l = first_line(lay_box_for_node(L, nw));
    CHECK_EQ(l->w, 8 * 5 + 6);
    tb = first_text(l);
    CHECK(tb && tb->text_len == 5);
    CHECK_EQ(tb->w, 8 * 5 + 6);

    /* pre "a  b": both preserved spaces are separators, +4 each. */
    l = first_line(lay_box_for_node(L, np));
    CHECK_EQ(l->w, 8 * 4 + 2 * 4);

    lay_free(L);
    tfree(t);
}

static void test_transform_layout(void)
{
    struct tctx *t = tnew();
    struct computed_style *root = st_block(t);
    struct computed_style *pop = st_block(t);
    struct computed_style *shf = st_block(t);
    struct computed_style *sib = st_block(t);
    struct computed_style *scl = st_block(t);
    struct dom_node *d, *np, *nsh, *nsib, *nsc;
    struct lay_document *L;
    struct lay_box *bp, *bsh, *bsib, *bsc;

    GROUP("transform: translateX(-50%), translateY(-2px), scale(2), hit-test");

    root->position = CSS_POSITION_RELATIVE;
    root->overflow = CSS_OVERFLOW_HIDDEN;
    root->width = LPX(400);
    root->height = LPX(400);

    pop->position = CSS_POSITION_ABSOLUTE;
    pop->offset[CSS_LEFT].type = CSS_LEN_PCT; pop->offset[CSS_LEFT].v = 50000;
    pop->offset[CSS_TOP] = LPX(10);
    pop->width = LPX(100);
    pop->height = LPX(40);
    pop->has_transform = 1;
    pop->translate_x.type = CSS_LEN_PCT; pop->translate_x.v = -50000;

    shf->width = LPX(80);
    shf->height = LPX(30);
    shf->margin[CSS_TOP] = LPX(20);
    shf->has_transform = 1;
    shf->translate_y = LPX(-2);

    sib->width = LPX(80);
    sib->height = LPX(20);

    scl->width = LPX(100);
    scl->height = LPX(40);
    scl->margin[CSS_TOP] = LPX(50);
    scl->margin[CSS_LEFT] = LPX(100);
    scl->has_transform = 1;
    scl->scale_x = 2000;
    scl->scale_y = 2000;

    d = el(t, 0, "div", root);
    np = el(t, d, "div", pop);
    nsh = el(t, d, "div", shf);
    nsib = el(t, d, "div", sib);
    nsc = el(t, d, "div", scl);

    L = run(t, d, 400, 400);
    bp = lay_box_for_node(L, np);
    bsh = lay_box_for_node(L, nsh);
    bsib = lay_box_for_node(L, nsib);
    bsc = lay_box_for_node(L, nsc);

    /* left:50% (200) + translateX(-50% of 100 = -50) => 150 */
    CHECK_EQ(bp->x, 150);
    CHECK_EQ(bp->y, 10);
    CHECK_EQ(lay_hit_test_view(L, 155, 20, 0, 0), bp);
    CHECK_EQ(lay_hit_test_view(L, 145, 20, 0, 0), lay_box_for_node(L, d));

    /* translateY(-2px) moves shf from 20 -> 18, sibling stays at 50 */
    CHECK_EQ(bsh->y, 18);
    CHECK_EQ(bsib->y, 50);

    /* scale(2) around center of (100, 120, 100, 40) -> (50, 100, 200, 80) */
    CHECK_EQ(bsc->x, 50);
    CHECK_EQ(bsc->y, 100);
    CHECK_EQ(bsc->w, 200);
    CHECK_EQ(bsc->h, 80);
    CHECK_EQ(lay_hit_test_view(L, 55, 110, 0, 0), bsc);

    lay_free(L);
    tfree(t);
}

/* ================================================================== *
 * main
 * ================================================================== */

int main(int argc, char **argv)
{
    int fuzz_iters = 3000;

    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "--stack") == 0) {
        stack_gate_mode = 1;
        printf("layout target-stack gate\n========================\n\n");
        test_pathological();
        printf("\n%ld checks, %ld failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--candidates") == 0) {
        printf("layout focused regression candidates\n"
               "====================================\n\n");
        test_percent_height();
        test_list_value_continuation();
        test_table_row_index_capacity();
        test_fixed_inside_relative();
        printf("\n%ld checks, %ld failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--row-boundary") == 0) {
        printf("layout table row-boundary gate\n"
               "==============================\n\n");
        test_table_row_boundary_runtime();
        printf("\n%ld checks, %ld failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--paint-oom") == 0) {
#ifdef LAYOUT_TEST_WRAP_ALLOC
        printf("layout paint-order OOM gate\n"
               "===========================\n\n");
        test_paint_order_oom();
        printf("\n%ld checks, %ld failures\n", checks, failures);
        return failures ? 1 : 0;
#else
        fprintf(stderr, "--paint-oom requires -DLAYOUT_TEST_WRAP_ALLOC "
                        "and -Wl,--wrap=malloc\n");
        return 2;
#endif
    }
    if (argc > 1)
        fuzz_iters = atoi(argv[1]);

    printf("layout engine tests\n===================\n\n");

    test_box_model();
    test_over_constrained();
    test_centred();
    test_percent_width();
    test_percent_height();
    test_min_max();

    test_collapse_siblings();
    test_collapse_parent_child();
    test_collapse_through();
    test_collapse_bfc();

    test_text_wrap();
    test_line_height();
    test_white_space();
    test_text_align();
    test_inline_boxes();
    test_vertical_align();
    test_inline_block();

    test_floats();
    test_float_containment();
    test_lists();
    test_list_value_continuation();

    test_table_auto();
    test_table_spacing_and_span();
    test_table_fixed();
    test_table_row_index_capacity();

    test_replaced();
    test_flex_row();
    test_flex_column();
    test_absolute();
    test_fixed_and_relative();
    test_fixed_inside_relative();
    test_zorder();

    test_overflow();
    test_hit_test();
    test_scroll_to_id();

    test_paint_svg();
    test_paint_basics();
    test_paint_clip();
    test_paint_text_and_scroll();
    test_paint_selection_caret();

    test_box_sizing_and_calc();
    test_flex_complete();
    test_grid();
    test_fixed_and_sticky_paint_hit();
    test_paint_modern_effects();
    test_paint_svg_modern();

    test_aspect_ratio_layout();
    test_object_fit_paint();
    test_flex_grid_modern();
    test_outline_paint();
    test_text_modern_layout();
    test_word_spacing();
    test_transform_layout();

    render_pages();
    test_pathological();
    test_performance();
    test_fuzz(fuzz_iters);

    printf("\n%ld checks, %ld failures\n", checks, failures);
    return failures ? 1 : 0;
}
