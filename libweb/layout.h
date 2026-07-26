/* layout.h - the CSS 2.1 visual formatting model for the Kestrel browser.
 *
 * Input:  a DOM tree whose elements carry a struct computed_style in
 *         dom_node.style (see css.h and style.c).
 * Output: a tree of struct lay_box with ABSOLUTE document coordinates,
 *         each box carrying its style, its originating DOM node, and -
 *         for text boxes - the exact byte run and the exact struct font
 *         that measured it. paint.c draws that tree; the browser hit
 *         tests it.
 *
 * What is implemented, in the order a page needs it:
 *
 *   box model      content/padding/border/margin, percentages against the
 *                  containing block, `auto` width and margin resolution
 *                  including margin:0 auto centring, min/max clamping.
 *   block flow     vertical stacking with margin collapsing in all three
 *                  forms (adjacent siblings, parent/first child,
 *                  collapse-through of empty boxes).
 *   inline flow    line boxes, word wrapping measured with font.h metrics,
 *                  white-space normal/pre/nowrap/pre-wrap/pre-line,
 *                  inline boxes with their own padding and border split
 *                  across lines, vertical-align, line-height, text-align
 *                  including justification.
 *   replaced       images with intrinsic sizes and aspect-ratio
 *                  preservation when only one dimension is given.
 *   lists          disc/circle/square/decimal/alpha/roman markers,
 *                  outside or inside.
 *   tables         measured min/max content widths per column, automatic
 *                  and fixed distribution, colspan and rowspan, cell
 *                  padding, borders, separate and collapsed borders.
 *   floats         left and right, line boxes shortened around them,
 *                  `clear`, float containment in a block formatting
 *                  context.
 *   positioning    relative offsets, absolute against the nearest
 *                  positioned ancestor's padding box, fixed against the
 *                  viewport, and a paint order that respects z-index.
 *   overflow       clip rectangles and the scrollable extent.
 *
 * PORTABILITY
 *   layout.c includes only <stdlib.h> and <string.h> plus this library's
 *   own headers, exactly like dom.c and css.c, so the same source builds
 *   against the host libc for tools/test_layout.c and against the
 *   KestrelOS freestanding libc for the browser. No floating point.
 *
 * MEMORY AND SAFETY
 *   Every box, string and side table is carved out of chunked arenas that
 *   are never reallocated, so box pointers are stable for the lifetime of
 *   the layout and freeing is a walk of the chunk list rather than of the
 *   tree. Every traversal that could be driven by document nesting is
 *   iterative or explicitly depth-capped at LAY_MAX_DEPTH, because the
 *   user stack is 64 KiB. Hitting any cap sets a bit in lay_truncated()
 *   and degrades the page; nothing here aborts.
 */

#pragma once

#include <stdint.h>

#include "css.h"

struct dom_node;
struct dom_document;
struct font;

/* ------------------------------------------------------------------ *
 * Limits
 * ------------------------------------------------------------------ */

#define LAY_MAX_BOXES        400000  /* boxes in one layout             */
#define LAY_MAX_DEPTH        96      /* box-tree nesting we will walk   */
#define LAY_MAX_FLOATS       512     /* live floats across all BFCs     */
#define LAY_MAX_INLINE_NEST  48      /* nested inline boxes on one line */
#define LAY_MAX_FRAGS        8192    /* fragments in one line box       */
#define LAY_MAX_COLUMNS      512     /* columns in one table            */
#define LAY_MAX_ROWS         100000  /* rows in one table               */
#define LAY_MAX_POSITIONED   16384   /* out-of-flow boxes per document  */
#define LAY_MAX_ARENA        (192UL * 1024UL * 1024UL)
#define LAY_MAX_ADJOIN       32      /* depth of one adjoining-margin
                                      * chain we will collapse through  */
#define LAY_MAX_LINES        200000  /* line boxes in one block         */

/* lay_truncated() bits */
#define LAY_TRUNC_BOXES      0x01u   /* box cap hit; content dropped    */
#define LAY_TRUNC_DEPTH      0x02u   /* depth cap hit; subtree dropped  */
#define LAY_TRUNC_MEMORY     0x04u   /* arena cap hit                   */
#define LAY_TRUNC_FLOATS     0x08u   /* float cap hit; floats inlined   */
#define LAY_TRUNC_TABLE      0x10u   /* table too wide/tall; clamped    */
#define LAY_TRUNC_POSITIONED 0x20u   /* out-of-flow cap hit             */
#define LAY_TRUNC_LINES      0x40u   /* line cap hit in one block       */
#define LAY_TRUNC_NEST       0x80u   /* inline nesting cap hit          */

/* ------------------------------------------------------------------ *
 * Box kinds
 *
 * A box is one of these; the kind decides which of the payload unions
 * below is meaningful and which layout algorithm owns it.
 * ------------------------------------------------------------------ */
enum {
    LAY_BOX_BLOCK = 0,     /* block container: children or line boxes   */
    LAY_BOX_INLINE,        /* one fragment of an inline element         */
    LAY_BOX_TEXT,          /* a glyph run: text/text_len/font are set   */
    LAY_BOX_LINE,          /* an anonymous line box inside a block      */
    LAY_BOX_REPLACED,      /* an image or other atomic replaced element */
    LAY_BOX_MARKER,        /* a list-item marker                        */
    LAY_BOX_TABLE,
    LAY_BOX_TABLE_ROWGROUP,
    LAY_BOX_TABLE_ROW,
    LAY_BOX_TABLE_CELL,
    LAY_BOX_KIND_COUNT
};

const char *lay_kind_name(int kind);

/* ------------------------------------------------------------------ *
 * Box flags
 * ------------------------------------------------------------------ */
#define LAYF_ANON        0x00000001u /* anonymous box; node is borrowed  */
#define LAYF_FLOAT       0x00000002u /* float: left|right                */
#define LAYF_POSITIONED  0x00000004u /* position != static               */
#define LAYF_ABSOLUTE    0x00000008u /* position: absolute or fixed      */
#define LAYF_FIXED       0x00000010u /* position: fixed                  */
#define LAYF_BFC         0x00000020u /* establishes a block formatting
                                      * context: contains its floats     */
#define LAYF_IFC         0x00000040u /* children are line boxes          */
#define LAYF_CLIP        0x00000080u /* overflow != visible: clips       */
#define LAYF_FIRST_FRAG  0x00000100u /* first fragment of an inline box:
                                      * draw its left border/padding     */
#define LAYF_LAST_FRAG   0x00000200u /* ... last: draw the right side    */
#define LAYF_LINK        0x00000400u /* inside an <a href>               */
#define LAYF_TRUNC       0x00000800u /* content dropped at a cap         */
#define LAYF_ATOMIC      0x00001000u /* atomic inline-level (inline-block,
                                      * replaced, inline-table)          */
#define LAYF_STACK       0x00002000u /* establishes a stacking context   */
#define LAYF_SCROLL      0x00004000u /* overflow scroll|auto: scrollable */
#define LAYF_HIDDEN      0x00008000u /* visibility: hidden               */
#define LAYF_PRE         0x00010000u /* white-space preserves spaces     */
#define LAYF_TABLE_ANON  0x00020000u /* anonymous table wrapper          */
#define LAYF_LAST_LINE   0x00040000u /* last line box of its block       */
#define LAYF_BR          0x00080000u /* the fragment came from <br>      */

/* ------------------------------------------------------------------ *
 * Rectangles
 * ------------------------------------------------------------------ */
typedef struct lay_rect {
    int32_t x, y, w, h;
} lay_rect;

static inline lay_rect lay_mkrect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    lay_rect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static inline int lay_rect_contains(lay_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline int lay_rect_intersects(lay_rect a, lay_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

lay_rect lay_rect_intersect(lay_rect a, lay_rect b);
lay_rect lay_rect_union(lay_rect a, lay_rect b);

/* ------------------------------------------------------------------ *
 * A box
 *
 * x/y/w/h are the CONTENT box in absolute document coordinates. The
 * padding, border and margin edges are derived with the helpers below;
 * keeping the content box canonical is what makes percentage resolution
 * and the tests read the same way as the specification does.
 * ------------------------------------------------------------------ */
struct lay_box {
    uint8_t  kind;             /* LAY_BOX_*                             */
    uint8_t  valign;           /* CSS_VALIGN_*, for line placement      */
    uint8_t  depth;            /* box-tree depth, 0 at the root         */
    uint8_t  pad0;
    uint32_t flags;            /* LAYF_*                                */

    int32_t  x, y, w, h;       /* content box, absolute                 */
    int32_t  pad[4];           /* CSS_TOP/RIGHT/BOTTOM/LEFT             */
    int32_t  bord[4];
    int32_t  marg[4];

    /* Union of this box's own painted area and every descendant's, so a
     * dirty-rectangle repaint can reject a whole subtree in O(1). */
    int32_t  ink_x, ink_y, ink_w, ink_h;

    /* The effective clip: this box's own clip rectangle intersected with
     * every clipping ancestor's. Painting never writes outside it. */
    int32_t  clip_x, clip_y, clip_w, clip_h;

    /* For LAYF_SCROLL boxes: the extent of the content, so the browser
     * knows how far it may scroll. Equals w/h when nothing overflows. */
    int32_t  scroll_w, scroll_h;

    int32_t  baseline;         /* line/inline/atomic: baseline from y    */
    int32_t  z;                /* used z-index (0 when auto)             */
    uint32_t order;            /* document order serial, for paint sort  */

    const struct computed_style *style;  /* never 0                      */
    struct dom_node *node;               /* 0 for anonymous boxes        */

    /* LAY_BOX_TEXT: a run of ASCII-folded, white-space-processed bytes
     * owned by the layout, plus the face that measured it. Painting this
     * run with this font reproduces the width layout assumed, exactly. */
    const char *text;
    uint32_t    text_len;
    const struct font *font;

    /* LAY_BOX_REPLACED: the resolved URL and the intrinsic size that was
     * used (0 when the image was not available). */
    const char *src;
    int32_t     intrinsic_w, intrinsic_h;

    /* LAY_BOX_MARKER: NUL-terminated marker text ("3.", "iv.", 0 for a
     * bullet glyph the painter draws itself). */
    const char *marker;

    /* Table cells: the grid slot they occupy. */
    uint16_t col, row, colspan, rowspan;

    struct lay_box *parent, *first_child, *last_child, *next, *prev;

    /* Intrusive list of the out-of-flow boxes whose containing block is
     * this box; used only during layout. */
    struct lay_box *sc_next;
};

/* ---- edges ---- */
static inline lay_rect lay_content_rect(const struct lay_box *b)
{
    return lay_mkrect(b->x, b->y, b->w, b->h);
}

static inline lay_rect lay_padding_rect(const struct lay_box *b)
{
    return lay_mkrect(b->x - b->pad[CSS_LEFT],
                      b->y - b->pad[CSS_TOP],
                      b->w + b->pad[CSS_LEFT] + b->pad[CSS_RIGHT],
                      b->h + b->pad[CSS_TOP] + b->pad[CSS_BOTTOM]);
}

static inline lay_rect lay_border_rect(const struct lay_box *b)
{
    return lay_mkrect(b->x - b->pad[CSS_LEFT] - b->bord[CSS_LEFT],
                      b->y - b->pad[CSS_TOP] - b->bord[CSS_TOP],
                      b->w + b->pad[CSS_LEFT] + b->pad[CSS_RIGHT] +
                          b->bord[CSS_LEFT] + b->bord[CSS_RIGHT],
                      b->h + b->pad[CSS_TOP] + b->pad[CSS_BOTTOM] +
                          b->bord[CSS_TOP] + b->bord[CSS_BOTTOM]);
}

static inline lay_rect lay_margin_rect(const struct lay_box *b)
{
    lay_rect r = lay_border_rect(b);

    r.x -= b->marg[CSS_LEFT];
    r.y -= b->marg[CSS_TOP];
    r.w += b->marg[CSS_LEFT] + b->marg[CSS_RIGHT];
    r.h += b->marg[CSS_TOP] + b->marg[CSS_BOTTOM];
    return r;
}

static inline lay_rect lay_ink_rect(const struct lay_box *b)
{
    return lay_mkrect(b->ink_x, b->ink_y, b->ink_w, b->ink_h);
}

static inline lay_rect lay_clip_rect(const struct lay_box *b)
{
    return lay_mkrect(b->clip_x, b->clip_y, b->clip_w, b->clip_h);
}

/* ------------------------------------------------------------------ *
 * Options
 *
 * The two callbacks are the modularity seam: layout never links an image
 * decoder and never links a CSS property that does not exist yet. A
 * caller that has neither passes zeroes and gets sensible fallbacks.
 * ------------------------------------------------------------------ */

/* Intrinsic size of an image. Return 1 and fill *w/*h, or 0 when the
 * image is not (yet) available - layout then falls back to the width and
 * height attributes, then to an alt-text sized placeholder. */
typedef int (*lay_image_size_fn)(void *ctx, const char *url, int *w, int *h);

/* 1 for `table-layout: fixed`, 0 for automatic. computed_style carries no
 * table-layout property today; when css.h grows one, wire it in here and
 * delete the heuristic in layout.c. */
typedef int (*lay_table_layout_fn)(void *ctx, struct dom_node *table);

struct lay_opts {
    int32_t viewport_w;         /* initial containing block width       */
    int32_t viewport_h;         /* ... and height; fixed boxes use it   */
    int32_t default_font_size;  /* px, for nodes with no computed style */
    unsigned long max_boxes;    /* 0 = LAY_MAX_BOXES                    */

    lay_image_size_fn   image_size;
    void               *image_ctx;
    lay_table_layout_fn table_layout;
    void               *table_ctx;
};

void lay_opts_init(struct lay_opts *o, int viewport_w, int viewport_h);

/* ------------------------------------------------------------------ *
 * Running layout
 * ------------------------------------------------------------------ */
struct lay_document;

/* Lay out `doc` (which must already be styled). Returns 0 only when the
 * very first allocation fails; every other failure is reported through
 * lay_truncated() and still yields a usable box tree. `o` may be 0, in
 * which case a 1024x768 viewport is assumed. */
struct lay_document *lay_layout(struct dom_document *doc,
                                const struct lay_opts *o);

/* Lay out an arbitrary subtree - used by the tests to avoid needing a
 * whole document, and by the browser for isolated fragments. `root` must
 * be an element node. */
struct lay_document *lay_layout_node(struct dom_node *root,
                                     const struct lay_opts *o);

void lay_free(struct lay_document *L);

struct lay_box *lay_root(const struct lay_document *L);   /* the ICB */
struct lay_box *lay_root_element(const struct lay_document *L);

int32_t lay_width(const struct lay_document *L);   /* scrollable extent */
int32_t lay_height(const struct lay_document *L);

unsigned long lay_box_count(const struct lay_document *L);
unsigned long lay_memory_used(const struct lay_document *L);
unsigned      lay_truncated(const struct lay_document *L);

/* The canvas colour: background-color propagated from <body> or <html>
 * per CSS 2.1 14.2, or 0xFFFFFFFF (opaque white) when neither sets one. */
uint32_t lay_canvas_color(const struct lay_document *L);

/* ------------------------------------------------------------------ *
 * Paint order
 *
 * The painter and the hit tester share one ordering so that what you
 * click is what you see. lay_paint_order() returns boxes in back-to-front
 * order; the array is owned by the layout and stays valid until it is
 * freed. Hit testing walks the same array backwards.
 * ------------------------------------------------------------------ */
struct lay_paint_item {
    struct lay_box *box;
    uint8_t phase;         /* LAY_PHASE_*                               */
};

enum {
    LAY_PHASE_BACKGROUND = 0, /* background and border of the box       */
    LAY_PHASE_CONTENT,        /* text, marker, replaced content         */
    LAY_PHASE_COUNT
};

int lay_paint_order(const struct lay_document *L,
                    const struct lay_paint_item **out);

/* ------------------------------------------------------------------ *
 * Hit testing and navigation
 * ------------------------------------------------------------------ */

/* Topmost box containing the document-space point, honouring paint order
 * and every clip rectangle, or 0. */
struct lay_box *lay_hit_test(const struct lay_document *L,
                             int32_t x, int32_t y);

/* The DOM node behind that point - the nearest non-anonymous ancestor of
 * the hit box - or 0. */
struct dom_node *lay_node_at(const struct lay_document *L,
                             int32_t x, int32_t y);

/* The <a href> under the point. Returns the anchor element and, when
 * `href` is non-null, its href attribute; 0 when the point is not on a
 * link. */
struct dom_node *lay_link_at(const struct lay_document *L,
                             int32_t x, int32_t y, const char **href);

/* The box generated by a DOM node - the first one, for an inline element
 * split across lines. O(1). */
struct lay_box *lay_box_for_node(const struct lay_document *L,
                                 const struct dom_node *n);

/* Document y of the element with this id, for fragment scrolling.
 * Returns 1 and stores the border-box top in *y, or 0. */
int lay_scroll_to_id(const struct lay_document *L, const char *id, int32_t *y);

/* Every box generated by one node, in order, via a callback. An inline
 * element that wraps across five lines has five fragments. */
typedef void (*lay_box_fn)(void *ctx, struct lay_box *b);
int lay_boxes_for_node(const struct lay_document *L, const struct dom_node *n,
                       lay_box_fn cb, void *ctx);

/* ------------------------------------------------------------------ *
 * Fonts
 *
 * Exposed because the painter must use exactly the face that measured a
 * run, and because the tests assert widths in terms of it.
 * ------------------------------------------------------------------ */

/* Map a CSS pixel font size onto one of the four bitmap faces. */
int lay_font_size_class(int32_t px);

/* The face a computed style resolves to. Never 0. */
const struct font *lay_font_for(const struct computed_style *cs);

/* The used line-height of a style in pixels. */
int32_t lay_line_height(const struct computed_style *cs);

/* ------------------------------------------------------------------ *
 * Diagnostics
 * ------------------------------------------------------------------ */
struct lay_stats {
    unsigned long boxes;
    unsigned long text_boxes;
    unsigned long line_boxes;
    unsigned long floats;
    unsigned long tables;
    unsigned long positioned;
    unsigned long max_depth;
    unsigned long stack_bytes;   /* deepest recursion measured, bytes   */
};

void lay_get_stats(const struct lay_document *L, struct lay_stats *out);
