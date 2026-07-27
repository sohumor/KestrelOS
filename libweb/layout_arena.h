/* layout_arena.h - private to layout.c and paint.c.
 *
 * The chunked arena the box tree lives in, plus the transient state the
 * layout pass threads through itself. Kept out of layout.h because none
 * of it is a contract: the browser sees boxes, not chunks.
 */

#pragma once

#include <stdint.h>

#include "layout.h"

/* ---- arena ------------------------------------------------------- */

struct lay_chunk {
    struct lay_chunk *next;
    unsigned long used, cap;
    /* payload follows */
};

/* ---- floats ------------------------------------------------------ */

struct lay_float {
    int32_t x, y, w, h;   /* margin box, absolute document coordinates */
    uint8_t side;         /* CSS_FLOAT_LEFT / CSS_FLOAT_RIGHT          */
};

/* ---- one fragment on the line under construction ----------------- */

enum { LAY_FRAG_TEXT = 0, LAY_FRAG_ATOMIC, LAY_FRAG_MARKER, LAY_FRAG_BR };

struct lay_frag {
    uint8_t  kind;
    uint8_t  valign;
    uint8_t  is_space;    /* a collapsible inter-word space            */
    uint8_t  pad0;
    int16_t  rec;         /* enclosing inline record, or -1            */
    int32_t  x, w;        /* relative to the line's content left edge  */
    int32_t  asc, desc;   /* extent above/below the baseline           */
    int32_t  asc0;        /* ... before vertical-align moved it        */
    int32_t  shift;       /* extra vertical offset (sub/super/length)  */
    const char *text;
    uint32_t len;
    const struct font *font;
    const struct computed_style *style;
    struct dom_node *node;
    struct lay_box *box;  /* atomic inline: the already-laid-out box   */
    uint32_t flags;
};

/* ---- one inline box's presence on one line ----------------------- */

struct lay_rec {
    int16_t parent;
    uint8_t open;
    uint8_t pad0;
    uint32_t flags;
    int32_t x0, x1;
    const struct computed_style *style;
    struct dom_node *node;
    struct lay_box *box;  /* filled in at commit time                  */
};

/* ---- a block formatting context ---------------------------------- */

struct lay_bfc {
    int      base;        /* first float index owned by this BFC       */
    int32_t  cx, cw;      /* the BFC root's content box x and width    */
    int32_t  low_left;    /* lowest bottom edge of a left float        */
    int32_t  low_right;
};

/* ---- collapsible margin ------------------------------------------ */

struct lay_marg {
    int32_t pos, neg;
};

/* ---- the layout document ----------------------------------------- */

struct lay_document {
    struct lay_chunk *chunks;
    unsigned long arena_bytes;

    struct dom_document *doc;
    struct lay_box *icb;        /* initial containing block            */
    struct lay_box *root_el;    /* the root element's box, or 0        */

    struct lay_opts opt;

    unsigned trunc;
    unsigned long nboxes;
    struct lay_stats stats;
    uint32_t canvas;
    int32_t doc_w, doc_h;

    /* node -> first box, open addressed, power-of-two sized */
    struct lay_box **hash;
    unsigned long hmask;

    struct lay_paint_item *paint;
    int npaint;
};

/* ---- the transient layout context -------------------------------- */

struct lay_ctx {
    struct lay_document *L;

    struct lay_float *floats;
    int nfl;

    struct lay_frag *frags;
    int nfrag;

    struct lay_rec *recs;
    int nrec;

    struct lay_box **posn;   /* out-of-flow boxes, laid out last       */
    int nposn;

    struct lay_box **rel;    /* position:relative boxes to offset      */
    int nrel;

    uint32_t order;
    int depth_max;
    uintptr_t stack_hi, stack_lo;   /* deepest recursion measured */
    int oom;
};

void *lay_arena_alloc(struct lay_document *L, unsigned long n);
char *lay_arena_str(struct lay_document *L, const char *s, unsigned long n);
