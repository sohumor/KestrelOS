/* paint.h - drawing a laid-out page into a 32-bit RGB surface.
 *
 * The painter walks the box tree in the paint order layout.c computed
 * (backgrounds, then floats, then inline content, then positioned boxes
 * by z-index) and draws backgrounds, borders, text, list markers and
 * images. It writes only inside the dirty rectangle it is given and only
 * inside each box's clip rectangle, so scrolling repaints the strip that
 * was exposed instead of the whole 2560x1440 window.
 *
 * The framebuffer has no alpha channel. Colours whose CSS alpha is zero
 * are treated as fully transparent (not drawn); any other alpha is
 * composited against what is already in the surface, which is exact for
 * opaque colours and a reasonable approximation otherwise.
 *
 * Like layout.c this file needs only <stdlib.h> and <string.h>.
 */

#pragma once

#include <stdint.h>

#include "layout.h"

struct image;
struct dom_node;

/* ------------------------------------------------------------------ *
 * The surface
 *
 * Deliberately the same shape as libgui's font_surface and gui_window's
 * pixel buffer, so a caller passes win->px straight through.
 * ------------------------------------------------------------------ */
struct paint_target {
    uint32_t *px;
    int32_t   w, h;
    int32_t   stride;   /* pixels per row; 0 means "tightly packed" */
};

/* ------------------------------------------------------------------ *
 * Hooks
 * ------------------------------------------------------------------ */

/* Decoded image for a URL, or 0 when it is not available - the painter
 * then draws the alt-text placeholder layout reserved space for. The
 * returned image is borrowed and must stay valid for the call. */
typedef const struct image *(*paint_image_fn)(void *ctx, const char *url);

/* Selection: for a text box, report which byte range of it is selected
 * and in what colours. Return 0 when nothing in this box is selected.
 * Offsets are byte offsets into box->text, clamped by the painter. */
typedef int (*paint_sel_fn)(void *ctx, const struct lay_box *text_box,
                            uint32_t *start, uint32_t *end,
                            uint32_t *fg, uint32_t *bg);

/* ------------------------------------------------------------------ *
 * Options
 * ------------------------------------------------------------------ */
struct paint_opts {
    /* Document point that lands at surface (0,0). Scrolling is a
     * translation, nothing more. */
    int32_t scroll_x, scroll_y;

    /* Where the document origin is drawn, in surface coordinates. Lets a
     * browser reserve a toolbar without offsetting every coordinate. */
    int32_t origin_x, origin_y;

    /* Filled behind everything. 0xAARRGGBB; alpha 0 means "do not fill",
     * which is what an incremental repaint over a valid backing store
     * wants. Normally lay_canvas_color(). */
    uint32_t canvas;

    paint_image_fn image;
    void *image_ctx;

    paint_sel_fn selection;
    void *selection_ctx;

    /* Text caret: drawn when caret_box is non-null, at `caret_offset`
     * bytes into that box's run. */
    const struct lay_box *caret_box;
    uint32_t caret_offset;
    uint32_t caret_color;

    /* Draw a focus/hover outline around this box, or 0. */
    const struct lay_box *highlight_box;
    uint32_t highlight_color;

    /* Colour links get when the style did not say (0 = use the style). */
    uint32_t link_color;

    /* Non-zero to draw one-pixel outlines around every box: the fastest
     * way to see what layout actually produced. */
    int debug_boxes;
};

void paint_opts_init(struct paint_opts *o);

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
struct paint_stats {
    unsigned long items_considered;
    unsigned long items_drawn;
    unsigned long glyphs;
    unsigned long rects;
    unsigned long images;
};

/* Paint the parts of `L` that fall inside `dirty` (surface coordinates).
 * Returns the number of paint items drawn. `stats` may be 0. Passing a
 * dirty rectangle of the whole surface repaints everything. */
int lay_paint(const struct lay_document *L, const struct paint_target *t,
              lay_rect dirty, const struct paint_opts *o,
              struct paint_stats *stats);

/* ------------------------------------------------------------------ *
 * Primitives, exposed because the browser chrome wants the same clipping
 * behaviour and the tests assert on them.
 * ------------------------------------------------------------------ */
void paint_fill(const struct paint_target *t, lay_rect clip, lay_rect r,
                uint32_t color);
void paint_frame(const struct paint_target *t, lay_rect clip, lay_rect r,
                 int32_t top, int32_t right, int32_t bottom, int32_t left,
                 const uint32_t *colors, const uint8_t *styles);

/* Blend `fg` over `bg` with coverage a (0..255). */
uint32_t paint_blend(uint32_t bg, uint32_t fg, unsigned a);

/* Write a P6 PPM of the surface. Returns 0 on success. Host tests use
 * this; it is compiled only when PAINT_WITH_STDIO is defined. */
int paint_write_ppm(const struct paint_target *t, const char *path);
