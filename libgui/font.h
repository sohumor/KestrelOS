#pragma once

/* libgui: text rendering at several sizes, weights and styles.
 *
 * Four faces are derived from one piece of hand-drawn art (see
 * libgui/mkfont_user.py):
 *
 *     FONT_SMALL   6x12    ascent  9   captions, footnotes, <small>
 *     FONT_BODY    8x16    ascent 12   body text and code -- the native
 *                                      bitmap, unchanged and unblurred
 *     FONT_LARGE   12x24   ascent 18   subheadings
 *     FONT_HUGE    16x32   ascent 24   headings
 *
 * The three derived faces store 4-bit coverage per pixel rather than one
 * bit, so their edges are antialiased: the blit blends the foreground
 * towards whatever is behind the glyph.  That is what makes scaled text
 * look like text rather than like a zoomed screenshot, and it is only
 * possible because the framebuffer is 32-bit.
 *
 * Bold and italic are synthetic and cost nothing in table space: bold
 * smears coverage horizontally by a column (two at 16x32), italic shears
 * the glyph 1:4 about the middle of the x-height.  Every size therefore
 * has all four combinations.
 *
 * This header depends on nothing but <stdint.h> -- no libc, no gui.h --
 * so the module compiles for the host and can be tested exhaustively
 * (tools/test_font.c) without booting anything.
 *
 * Typical use from a libgui program:
 *
 *     font_surface fs = font_surface_of(win->px, win->w, win->h, win->w);
 *     font_rc      cl = font_mkrc(win->clip.x, win->clip.y,
 *                                 win->clip.w, win->clip.h);
 *     const struct font *h1 = font_get(FONT_HUGE, FONT_BOLD, FONT_ROMAN);
 *
 *     font_draw_text(&fs, cl, 8, 8, "Heading", h1, GUI_TEXT,
 *                    FONT_TRANSPARENT);
 */

#include <stdint.h>

/* ------------------------------------------------------------- selectors */

enum font_size {
    FONT_SMALL = 0,   /* 6x12  */
    FONT_BODY  = 1,   /* 8x16  */
    FONT_LARGE = 2,   /* 12x24 */
    FONT_HUGE  = 3,   /* 16x32 */
    FONT_SIZE_COUNT = 4
};

enum font_weight {
    FONT_REGULAR = 0,
    FONT_BOLD    = 1,
    FONT_WEIGHT_COUNT = 2
};

enum font_style {
    FONT_ROMAN  = 0,
    FONT_ITALIC = 1,
    FONT_STYLE_COUNT = 2
};

/* bg argument to the drawing calls: >= 0 is an opaque 0x00RRGGBB colour,
 * this is "blend the glyph into whatever is already there".  Same value
 * and meaning as GUI_TRANSPARENT. */
#define FONT_TRANSPARENT (-1L)

/* --------------------------------------------------------------- a face */

struct font {
    const char *name;         /* "8x16 bold italic"                     */
    const uint8_t *cov;       /* 4-bit coverage, or 0 for the 1-bit face */
    const uint8_t *bits;      /* 1-bit rows, or 0 for a coverage face    */
    int16_t w, h;             /* cell size; w is also the advance        */
    int16_t ascent, descent;  /* h == ascent + descent                   */
    int16_t cap, xheight;     /* cap line and x line, above the baseline */
    int16_t smear;            /* synthetic-bold coverage smear, columns  */
    int16_t smear_gain;       /* ... at this strength, 0..256            */
    int16_t shear_pivot;      /* row the italic shear rotates about      */
    int16_t shear_den;        /* dx = (pivot - row) / den, 0 = upright   */
    uint8_t size, weight, style;
};

/* Pick a face.  Out-of-range selectors are clamped rather than refused,
 * so this never returns NULL and a caller can pass a computed size
 * without checking it first. */
const struct font *font_get(int size, int weight, int style);

/* The face whose line height is closest to px_h (ties round down), with
 * the given weight and style.  Handy for a stylesheet that asks for a
 * pixel size rather than one of the four names. */
const struct font *font_for_height(int px_h, int weight, int style);

/* ------------------------------------------------------------- metrics */

/* Advance of one character in pixels.  Every face is monospaced, so this
 * is f->w for every drawable character; '\0' is zero width and draws
 * nothing, and anything else outside 0x20..0x7E draws the fallback box.
 * Passing a char with the high bit set is safe: c is taken modulo 256. */
int font_char_width(const struct font *f, int c);

int font_line_height(const struct font *f);
int font_ascent(const struct font *f);
int font_descent(const struct font *f);

/* Width of a string.  n < 0 means "up to the NUL"; otherwise at most n
 * characters, still stopping at a NUL.  A null pointer measures 0. */
int font_text_width(const struct font *f, const char *s);
int font_text_width_n(const struct font *f, const char *s, int n);

/* Largest prefix of s that fits in max_w pixels (max_w <= 0 = unlimited).
 * Returns the number of characters; *out_w, when non-null, receives the
 * width of that prefix. */
int font_fit(const struct font *f, const char *s, int n, int max_w,
             int *out_w);

/* --------------------------------------------------------------- output */

typedef struct font_rc {
    int x, y, w, h;
} font_rc;

/* A 32-bit 0x00RRGGBB pixel buffer: a window, an offscreen page, a test
 * canvas.  stride is pixels per row (0 means "tightly packed", i.e. w). */
typedef struct font_surface {
    uint32_t *px;
    int w, h;
    int stride;
} font_surface;

static inline font_rc font_mkrc(int x, int y, int w, int h)
{
    font_rc r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static inline font_surface font_surface_of(uint32_t *px, int w, int h,
                                           int stride)
{
    font_surface s;

    s.px = px;
    s.w = w;
    s.h = h;
    s.stride = stride;
    return s;
}

/* Blend `fg` over `bg` with coverage a (0..255). */
uint32_t font_blend(uint32_t bg, uint32_t fg, unsigned a);

/* Draw one glyph with its cell's top-left corner at (x, y), clipped to
 * `clip` intersected with the surface.  When bg >= 0 the w*h cell is
 * filled with it first; with FONT_TRANSPARENT the glyph blends into the
 * existing pixels.  Returns the advance, so a caller can chain.
 *
 * Italic ink leans out of its own cell by roughly h/8 pixels on each
 * side, which is what makes consecutive italics look joined-up; the
 * clip rectangle still contains it, so nothing can be scribbled outside
 * the caller's box.  For that reason a run drawn with an opaque bg fills
 * the whole run's background in one go instead of cell by cell -- see
 * font_draw_run. */
int font_draw_char(const font_surface *dst, font_rc clip, int x, int y,
                   int c, const struct font *f, uint32_t fg, long bg);

/* Draw a string, returning its advance in pixels.  n < 0 means "up to the
 * NUL".  Stops early at a NUL either way. */
int font_draw_text_n(const font_surface *dst, font_rc clip, int x, int y,
                     const char *s, int n, const struct font *f,
                     uint32_t fg, long bg);
int font_draw_text(const font_surface *dst, font_rc clip, int x, int y,
                   const char *s, const struct font *f, uint32_t fg,
                   long bg);

/* What a run did, filled in by font_measure_run/font_draw_run. */
typedef struct font_run {
    int width;      /* pixels the run occupies, ellipsis included    */
    int chars;      /* characters of s that were used                */
    int truncated;  /* non-zero if s did not fit                     */
    int ellipsis;   /* dots appended, 0..3 (fewer only when even the
                     * ellipsis is wider than the space available)   */
} font_run;

/* Lay out at most n characters of s in max_w pixels (max_w <= 0 =
 * unlimited).  When `ellipsis` is non-zero and the string does not fit,
 * room is kept for "..." and the text is cut back to make it; if even the
 * ellipsis does not fit, as much of it as possible is drawn.  Measuring
 * and drawing use the same code path, so what fits is what appears. */
void font_measure_run(const struct font *f, const char *s, int n,
                      int max_w, int ellipsis, font_run *out);

/* Draw such a run and return its advance.  `out` may be null. */
int font_draw_run(const font_surface *dst, font_rc clip, int x, int y,
                  const char *s, int n, const struct font *f,
                  uint32_t fg, long bg, int max_w, int ellipsis,
                  font_run *out);

/* --------------------------------------------------- generated tables */

/* The native 8x16 face, shared with draw.c (gui.h declares these too). */
extern const uint8_t gui_font8x16[95][16];
extern const uint8_t gui_font8x16_fallback[16];

/* 4-bit coverage, two pixels per byte, low nibble = even x, 96 cells of
 * w*h/2 bytes (ASCII 0x20..0x7E then the fallback). */
extern const uint8_t gui_font_cov6x12[3456];
extern const uint8_t gui_font_cov12x24[13824];
extern const uint8_t gui_font_cov16x32[24576];
