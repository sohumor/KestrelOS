/* html.h - HTML subset parser and layout engine for the Kestrel browser.
 *
 * Deliberately free of every KestrelOS dependency: it uses only malloc,
 * realloc, free and the standard mem/str routines, so the same source
 * compiles against the host libc for unit tests (tools/html_test.c) and
 * against KestrelOS libc for apps/browser.c.
 *
 * Two stages:
 *
 *   html_parse()        bytes -> struct html_doc, a FLAT list of boxes
 *                       (text runs carrying a style, block breaks, list
 *                       bullets, rules, image placeholders). No tree, no
 *                       geometry.
 *   html_layout_build() boxes + a width + a font metric callback ->
 *                       positioned lines and runs. The metric callback is
 *                       the only thing that knows about fonts, so the same
 *                       layout code drives the framebuffer (8 by 16
 *                       pixel glyphs) and the serial console (character
 *                       cells one unit square).
 *
 * Supported elements: h1-h6, p, br, hr, a, b, strong, i, em, u, ul, ol,
 * li, pre, code, blockquote, div, span, table/tr/td/th, img, dl/dt/dd and
 * the usual HTML5 sectioning wrappers. Unknown elements are transparent:
 * the tag is dropped, the content is kept. script and style content is
 * discarded entirely.
 *
 * NOT supported, on purpose: CSS, JavaScript, floats, real table layout
 * (tables degrade to one line of "cell | cell | cell" per row), forms,
 * images (an outlined placeholder holding the alt text is emitted), and
 * anything past US-ASCII (non-ASCII entities fold to an ASCII lookalike
 * or '?').
 */

#pragma once

/* ---- inline style bits (struct html_box.style) ---- */
#define HS_BOLD   0x01u
#define HS_ITALIC 0x02u
#define HS_UNDER  0x04u
#define HS_MONO   0x08u
#define HS_LINK   0x10u
#define HS_PRE    0x20u   /* preformatted: no wrapping, spaces preserved */

/* ---- box kinds (struct html_box.kind) ---- */
#define HB_TEXT   0   /* .text is a run of text in .style */
#define HB_BREAK  1   /* end the line (br, table row, list item) */
#define HB_BLOCK  2   /* end the line and leave a paragraph gap */
#define HB_RULE   3   /* <hr>: a full-width horizontal rule */
#define HB_BULLET 4   /* list marker in .text ("* " or "12. ") */
#define HB_IMAGE  5   /* <img>: placeholder, .text is the alt text */

/* Hard limits. A hostile or merely enormous page is truncated, never
 * allowed to exhaust the heap; html_doc.truncated records that. */
#define HTML_MAX_INPUT  (1024UL * 1024UL)
#define HTML_MAX_BOXES  200000

struct html_box {
    int kind;             /* HB_* */
    unsigned int style;   /* HS_* bits */
    int heading;          /* 0, or 1..6 inside h1..h6 */
    int indent;           /* nesting level: lists and blockquotes */
    const char *text;     /* NUL-terminated, owned by the doc, or 0 */
    const char *href;     /* link target when style & HS_LINK, else 0 */
};

struct html_doc {
    struct html_box *boxes;
    int nbox;
    const char *title;    /* <title> text, or 0 */
    int truncated;        /* input or box count hit a limit */
    char *arena;          /* private: string storage for text/href/title */
};

/* Parse len bytes of HTML. Never fails on malformed input; returns 0 only
 * when out of memory. The result owns all its strings. */
struct html_doc *html_parse(const char *src, unsigned long len);
void html_free(struct html_doc *d);

/* ---- layout ---- */

/* Font metrics. text_w measures the first len bytes of s in the given
 * style; line_h gives the line box height for that style. Both take the
 * heading level so a renderer can make headings heavier or taller. */
struct html_metrics {
    int (*text_w)(void *ctx, const char *s, int len,
                  unsigned int style, int heading);
    int (*line_h)(void *ctx, unsigned int style, int heading);
    void *ctx;
    int indent_w;   /* width of one indent level */
    int para_gap;   /* extra vertical space at a HB_BLOCK */
};

struct html_run {
    int x, y, w, h;
    int kind;             /* HB_TEXT / HB_RULE / HB_BULLET / HB_IMAGE */
    unsigned int style;
    int heading;
    const char *text;     /* NUL-terminated, owned by the layout */
    const char *href;     /* borrowed from the doc; valid while it lives */
    int box;              /* index of the source box */
};

struct html_line {
    int y, h;
    int first, count;     /* range in layout->runs */
};

struct html_layout {
    struct html_run *runs;
    int nrun;
    struct html_line *lines;
    int nline;
    int width;            /* the width it was laid out for */
    int height;           /* total height produced */
    char *arena;          /* private: run text storage */
};

/* Lay the document out into `width` units. Returns 0 on allocation
 * failure. `width` is clamped to at least one indent step. */
struct html_layout *html_layout_build(const struct html_doc *d, int width,
                                      const struct html_metrics *m);
void html_layout_free(struct html_layout *l);

/* Index of the run containing (x, y), or -1. Used for link hit-testing. */
int html_layout_hit(const struct html_layout *l, int x, int y);

/* Metrics for a character-cell device: one column per character, one row
 * per line, two columns per indent level, one blank row between blocks. */
void html_metrics_chars(struct html_metrics *m);
