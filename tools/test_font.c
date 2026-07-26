/* Host test harness for libgui/font.c.
 *
 * The font module is pure computation over a caller-supplied pixel
 * buffer: no syscalls, no libc, no I/O.  So it compiles for the host and
 * can be tested exhaustively without booting KestrelOS, which is the only
 * way to check a visual component honestly -- the unit tests below prove
 * the metrics and the clipping, and the proof sheets prove the glyphs
 * actually look like letters.
 *
 *     gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibgui \
 *         -o /tmp/test_font tools/test_font.c libgui/font.c libgui/font_data.c
 *     /tmp/test_font              # unit tests only
 *     /tmp/test_font /tmp/sheets  # ... and write the proof sheets
 *
 * The sheets come out as binary PPM; tools/ppm2png.py turns them into
 * PNGs.
 */

#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- harness */

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void checkf(int ok, const char *fmt, long a, long b)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: ");
        printf(fmt, a, b);
        printf("\n");
    }
}

/* ------------------------------------------------------------- canvases */

#define SENTINEL 0x00AAAAAAu

typedef struct canvas {
    font_surface s;
    uint32_t *mem;
    int stride;
} canvas;

static canvas canvas_new(int w, int h, int stride, uint32_t fillc)
{
    canvas c;
    long i, n;

    if (stride <= 0)
        stride = w;
    n = (long)stride * h;
    c.mem = malloc((size_t)n * sizeof(uint32_t));
    if (!c.mem) {
        printf("out of memory\n");
        exit(2);
    }
    for (i = 0; i < n; i++)
        c.mem[i] = fillc;
    c.stride = stride;
    c.s = font_surface_of(c.mem, w, h, stride);
    return c;
}

static void canvas_free(canvas *c)
{
    free(c->mem);
    c->mem = 0;
    c->s.px = 0;
}

static uint32_t at(const canvas *c, int x, int y)
{
    return c->mem[(long)y * c->stride + x];
}

/* Every pixel outside `keep` must still hold the sentinel. */
static int only_inside(const canvas *c, font_rc keep)
{
    int x, y;

    for (y = 0; y < c->s.h; y++)
        for (x = 0; x < c->stride; x++) {
            int in = x >= keep.x && x < keep.x + keep.w &&
                     y >= keep.y && y < keep.y + keep.h && x < c->s.w;

            if (!in && at(c, x, y) != SENTINEL)
                return 0;
        }
    return 1;
}

static int count_ink(const canvas *c, uint32_t bg)
{
    int x, y, n = 0;

    for (y = 0; y < c->s.h; y++)
        for (x = 0; x < c->s.w; x++)
            if (at(c, x, y) != bg)
                n++;
    return n;
}

/* --------------------------------------------------------- unit tests */

static const char *const size_name[] = { "6x12", "8x16", "12x24", "16x32" };
static const int size_w[] = { 6, 8, 12, 16 };
static const int size_h[] = { 12, 16, 24, 32 };

static void test_faces(void)
{
    int sz, wt, st;

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
        for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
            for (st = 0; st < FONT_STYLE_COUNT; st++) {
                const struct font *f = font_get(sz, wt, st);

                checkf(f->w == size_w[sz], "%s: width %ld", (long)sz, f->w);
                checkf(f->h == size_h[sz], "%s: height %ld", (long)sz, f->h);
                check(f->ascent + f->descent == f->h,
                      "ascent + descent == line height");
                check(f->ascent > f->cap && f->cap > f->xheight,
                      "cap line sits between the x line and the ascent");
                check(f->size == sz && f->weight == wt && f->style == st,
                      "face reports its own selectors");
                check(font_line_height(f) == f->h, "font_line_height");
                check(font_ascent(f) == f->ascent, "font_ascent");
                check(font_descent(f) == f->descent, "font_descent");
                check((f->cov != 0) != (f->bits != 0),
                      "a face has coverage or bits, never both");
                if (wt == FONT_REGULAR)
                    check(f->smear == 0, "regular weight has no smear");
                else
                    check(f->smear >= 1, "bold weight smears");
                if (st == FONT_ROMAN)
                    check(f->shear_den == 0, "roman does not shear");
                else
                    check(f->shear_den == 4, "italic shears 1:4");
                check(strstr(f->name, size_name[sz]) != 0,
                      "face name carries its size");
            }

    /* Clamping, not crashing, on nonsense selectors. */
    check(font_get(-5, -5, -5) == font_get(0, 0, 0), "selectors clamp low");
    check(font_get(99, 99, 99) ==
              font_get(FONT_SIZE_COUNT - 1, FONT_WEIGHT_COUNT - 1,
                       FONT_STYLE_COUNT - 1), "selectors clamp high");

    check(font_for_height(12, FONT_REGULAR, FONT_ROMAN)->h == 12,
          "font_for_height exact 12");
    check(font_for_height(16, FONT_REGULAR, FONT_ROMAN)->h == 16,
          "font_for_height exact 16");
    check(font_for_height(1, FONT_REGULAR, FONT_ROMAN)->h == 12,
          "font_for_height floor");
    check(font_for_height(1000, FONT_BOLD, FONT_ITALIC)->h == 32,
          "font_for_height ceiling");
    check(font_for_height(1000, FONT_BOLD, FONT_ITALIC)->weight == FONT_BOLD,
          "font_for_height keeps the weight");
    check(font_for_height(20, FONT_REGULAR, FONT_ROMAN)->h == 16,
          "font_for_height nearest, ties low");
    check(font_for_height(-2000000000, 0, 0)->h == 12,
          "font_for_height survives a nonsense size");
    check(font_for_height(2000000000, 0, 0)->h == 32,
          "font_for_height survives a huge size");
}

static void test_metrics(void)
{
    const struct font *f = font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN);
    int c, w;

    for (c = 1; c < 256; c++)
        checkf(font_char_width(f, c) == 8, "char %ld width %ld",
               (long)c, (long)font_char_width(f, c));
    check(font_char_width(f, 0) == 0, "NUL is zero width");
    check(font_char_width(f, 256) == 0, "256 folds onto NUL");
    check(font_char_width(0, 'a') == 0, "null face is zero width");

    check(font_text_width(f, "") == 0, "empty string is zero wide");
    check(font_text_width(f, 0) == 0, "null string is zero wide");
    check(font_text_width(f, "hello") == 40, "5 chars at 8px");
    check(font_text_width_n(f, "hello", 3) == 24, "n limits the count");
    check(font_text_width_n(f, "hi", 99) == 16, "the NUL limits the count");
    check(font_text_width_n(f, "hello", 0) == 0, "n == 0 measures nothing");
    check(font_text_width_n(f, "hello", -1) == 40, "n < 0 is the whole string");

    check(font_fit(f, "hello", -1, 0, &w) == 0 && w == 0, "fit in 0px");
    check(font_fit(f, "hello", -1, 7, &w) == 0 && w == 0, "fit in 7px");
    check(font_fit(f, "hello", -1, 8, &w) == 1 && w == 8, "fit in 8px");
    check(font_fit(f, "hello", -1, 39, &w) == 4 && w == 32, "fit in 39px");
    check(font_fit(f, "hello", -1, 4000, &w) == 5 && w == 40, "fit, room over");
    check(font_fit(f, "hello", -1, -1, &w) == 5 && w == 40, "fit unlimited");
    check(font_fit(f, 0, -1, 100, &w) == 0 && w == 0, "fit null string");
    check(font_fit(f, "hello", 2, 100, 0) == 2, "fit honours n");
    check(font_fit(0, "hello", -1, 100, &w) == 0 && w == 0, "fit null face");

    /* Synthetic bold and italic must be declared consistently, since the
     * blit trusts them without checking. */
    {
        int sz;

        for (sz = 0; sz < FONT_SIZE_COUNT; sz++) {
            const struct font *b = font_get(sz, FONT_BOLD, FONT_ROMAN);
            const struct font *it = font_get(sz, FONT_REGULAR, FONT_ITALIC);

            check(b->smear_gain > 0 && b->smear_gain <= 256,
                  "smear gain is a sane 0..256 fraction");
            check(b->smear < b->w, "the bold smear stays inside the cell");
            check(it->shear_pivot > 0 && it->shear_pivot < it->h,
                  "the italic pivot is inside the cell");
        }
    }

    /* All four sizes agree that width scales with the face. */
    check(font_text_width(font_get(FONT_SMALL, 0, 0), "abcd") == 24, "6x12 x4");
    check(font_text_width(font_get(FONT_LARGE, 0, 0), "abcd") == 48, "12x24 x4");
    check(font_text_width(font_get(FONT_HUGE, 0, 0), "abcd") == 64, "16x32 x4");
    /* Bold and italic keep the advance, so layout does not reflow. */
    check(font_text_width(font_get(FONT_BODY, FONT_BOLD, FONT_ITALIC),
                          "abcd") == 32, "bold italic keeps the advance");
}

static void test_runs(void)
{
    const struct font *f = font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN);
    font_run r;

    font_measure_run(f, "hello", -1, -1, 0, &r);
    check(r.width == 40 && r.chars == 5 && !r.truncated && !r.ellipsis,
          "unlimited run");

    font_measure_run(f, "hello", -1, 40, 1, &r);
    check(r.width == 40 && r.chars == 5 && !r.truncated,
          "exactly-fitting run keeps every character");

    font_measure_run(f, "hello world", -1, 40, 0, &r);
    check(r.width == 40 && r.chars == 5 && r.truncated && !r.ellipsis,
          "hard truncation");

    font_measure_run(f, "hello world", -1, 40, 1, &r);
    check(r.width == 40 && r.chars == 2 && r.truncated && r.ellipsis == 3,
          "ellipsis eats three cells");

    font_measure_run(f, "hello world", -1, 24, 1, &r);
    check(r.width == 24 && r.chars == 0 && r.ellipsis == 3,
          "just enough room for the ellipsis alone");

    font_measure_run(f, "hello world", -1, 23, 1, &r);
    check(r.width == 16 && r.chars == 0 && r.ellipsis == 2 && r.truncated,
          "too little room: as many dots as fit");

    font_measure_run(f, "hello world", -1, 7, 1, &r);
    check(r.width == 0 && r.chars == 0 && r.ellipsis == 0 && r.truncated,
          "no room at all");

    font_measure_run(f, "", -1, 100, 1, &r);
    check(r.width == 0 && r.chars == 0 && !r.truncated, "empty run");

    font_measure_run(f, 0, -1, 100, 1, &r);
    check(r.width == 0 && !r.truncated, "null run");

    font_measure_run(f, "hello", 2, -1, 0, &r);
    check(r.width == 16 && r.chars == 2, "n limits the run");

    font_measure_run(f, "hello", -1, 0, 1, &r);
    check(r.width == 0 && r.truncated, "zero width fits nothing");

    font_measure_run(0, "hello", -1, -1, 0, &r);
    check(r.width == 0, "null face measures nothing");
    font_measure_run(f, "hello", -1, -1, 0, 0);   /* must not crash */
}

static void test_blend(void)
{
    check(font_blend(0x000000, 0xFFFFFF, 0) == 0x000000, "alpha 0 keeps bg");
    check(font_blend(0x000000, 0xFFFFFF, 255) == 0xFFFFFF, "alpha 255 is fg");
    check(font_blend(0x000000, 0xFFFFFF, 128) == 0x808080, "half way");
    check(font_blend(0xFFFFFF, 0x000000, 128) == 0x7F7F7F, "half way back");
    check(font_blend(0x102030, 0x102030, 77) == 0x102030, "same colour");
    check((font_blend(0xFF00FF, 0x00FF00, 60) & 0xFF000000u) == 0,
          "the top byte stays clear");
    {
        unsigned a;

        for (a = 0; a <= 255; a++) {
            uint32_t v = font_blend(0x000000, 0xFFFFFF, a);
            unsigned r = (v >> 16) & 0xFF;

            checkf(r == a, "blend %ld gave %ld", (long)a, (long)r);
        }
    }
}

/* Nothing may be written outside the clip rectangle, from any position,
 * at any size, in any style, with either background mode. */
static void test_clipping(void)
{
    const int W = 80, H = 80;
    font_rc clip = { 20, 20, 30, 30 };
    int sz, wt, st, i;
    static const int pos[] = { -40, -17, -1, 0, 1, 19, 20, 21, 35,
                               48, 49, 50, 51, 79, 80, 120 };
    const int npos = (int)(sizeof(pos) / sizeof(pos[0]));

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
        for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
            for (st = 0; st < FONT_STYLE_COUNT; st++) {
                const struct font *f = font_get(sz, wt, st);
                int a, b, bgmode;

                for (bgmode = 0; bgmode < 2; bgmode++)
                    for (a = 0; a < npos; a++)
                        for (b = 0; b < npos; b++) {
                            canvas c = canvas_new(W, H, W + 13, SENTINEL);
                            long bg = bgmode ? 0x123456L : FONT_TRANSPARENT;

                            font_draw_text(&c.s, clip, pos[a], pos[b],
                                           "Wq|#", f, 0xFFFFFF, bg);
                            check(only_inside(&c, clip),
                                  "text drew outside the clip rectangle");
                            font_draw_char(&c.s, clip, pos[a], pos[b],
                                           'W', f, 0xFFFFFF, bg);
                            check(only_inside(&c, clip),
                                  "char drew outside the clip rectangle");
                            font_draw_run(&c.s, clip, pos[a], pos[b],
                                          "long enough to truncate", -1, f,
                                          0xFFFFFF, bg, 40, 1, 0);
                            check(only_inside(&c, clip),
                                  "run drew outside the clip rectangle");
                            canvas_free(&c);
                        }
            }

    /* A clip rectangle bigger than the surface is trimmed to it. */
    for (i = 0; i < 2; i++) {
        canvas c = canvas_new(40, 40, 57, SENTINEL);
        font_rc huge = { -100, -100, 1000, 1000 };
        font_rc all = { 0, 0, 40, 40 };

        font_draw_text(&c.s, huge, i ? -3 : 30, 30, "MMMM",
                       font_get(FONT_HUGE, FONT_BOLD, FONT_ITALIC),
                       0xFFFFFF, i ? FONT_TRANSPARENT : 0x111111L);
        check(only_inside(&c, all), "an oversized clip is trimmed");
        canvas_free(&c);
    }

    /* Degenerate clips draw nothing at all. */
    {
        static const font_rc bad[] = {
            { 0, 0, 0, 10 }, { 0, 0, 10, 0 }, { 0, 0, -5, -5 },
            { 100, 100, 10, 10 }, { -50, -50, 10, 10 }
        };
        unsigned k;

        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            canvas c = canvas_new(40, 40, 40, SENTINEL);

            font_draw_text(&c.s, bad[k], 0, 0, "hello",
                           font_get(FONT_BODY, 0, 0), 0xFFFFFF, 0x000000L);
            check(count_ink(&c, SENTINEL) == 0,
                  "a degenerate clip draws nothing");
            canvas_free(&c);
        }
    }
}

/* The advance is returned even when there is nowhere to draw, so a
 * caller laying out text into an offscreen or clipped-away region still
 * gets the same geometry. */
static void test_degenerate_surfaces(void)
{
    const struct font *f = font_get(FONT_BODY, 0, 0);
    font_rc clip = { 0, 0, 100, 100 };
    font_surface s;
    uint32_t one = 0;

    check(font_draw_text(0, clip, 0, 0, "hello", f, 0xFFFFFF, -1) == 40,
          "null surface still advances");
    check(font_draw_char(0, clip, 0, 0, 'x', f, 0xFFFFFF, -1) == 8,
          "null surface still advances one char");

    s = font_surface_of(0, 10, 10, 10);
    check(font_draw_text(&s, clip, 0, 0, "hello", f, 0xFFFFFF, -1) == 40,
          "null pixels still advance");

    s = font_surface_of(&one, 0, 0, 0);
    check(font_draw_text(&s, clip, 0, 0, "hello", f, 0xFFFFFF, -1) == 40,
          "empty surface still advances");
    check(one == 0, "empty surface untouched");

    /* A stride narrower than the surface is a caller bug; refuse it
     * rather than run off the end of every row. */
    s = font_surface_of(&one, 40, 1, 3);
    check(font_draw_text(&s, clip, 0, 0, "hello", f, 0xFFFFFF, -1) == 40,
          "bad stride still advances");
    check(one == 0, "bad stride draws nothing");

    check(font_draw_char(0, clip, 0, 0, 0, f, 0xFFFFFF, -1) == 0,
          "NUL draws nothing and advances nothing");
    check(font_draw_text(0, clip, 0, 0, 0, f, 0xFFFFFF, -1) == 0,
          "null string advances nothing");
    check(font_draw_char(0, clip, 0, 0, 'x', 0, 0xFFFFFF, -1) == 0,
          "null face advances nothing");

    /* Absurd coordinates -- what a broken document can hand a layout
     * engine -- must be refused rather than wrapped around. */
    {
        static const int wild[] = { -2000000000, -20000000, 20000000,
                                    2000000000 };
        unsigned i, j;

        for (i = 0; i < sizeof(wild) / sizeof(wild[0]); i++) {
            canvas cv = canvas_new(40, 40, 40, SENTINEL);
            font_rc allrc = { 0, 0, 40, 40 };

            for (j = 0; j < sizeof(wild) / sizeof(wild[0]); j++) {
                font_draw_text(&cv.s, allrc, wild[i], wild[j], "hello",
                               f, 0xFFFFFF, 0x000000L);
                font_draw_text(&cv.s, allrc, 4, 4, "hello", f, 0xFFFFFF,
                               FONT_TRANSPARENT);
                font_draw_char(&cv.s,
                               font_mkrc(wild[i], wild[j], wild[i], wild[j]),
                               2, 2, 'x', f, 0xFFFFFF, 0x000000L);
            }
            check(count_ink(&cv, SENTINEL) > 0,
                  "sane coordinates still draw next to wild ones");
            canvas_free(&cv);
        }
    }
}

/* Every printable character must put ink on the page, the fallback box
 * must be drawn for everything outside the range, and the space must be
 * blank. */
static void test_coverage(void)
{
    int sz, wt, st, c;

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
        for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
            for (st = 0; st < FONT_STYLE_COUNT; st++) {
                const struct font *f = font_get(sz, wt, st);
                font_rc all = { 0, 0, 60, 60 };

                for (c = 1; c < 256; c++) {
                    canvas cv = canvas_new(60, 60, 60, 0x000000);
                    int ink;

                    font_draw_char(&cv.s, all, 20, 20, c, f, 0xFFFFFF, -1);
                    ink = count_ink(&cv, 0x000000);
                    if (c == ' ')
                        checkf(ink == 0, "space inked %ld px (size %ld)",
                               (long)ink, (long)sz);
                    else
                        checkf(ink > 0, "char %ld drew nothing (size %ld)",
                               (long)c, (long)sz);
                    canvas_free(&cv);
                }
            }

    /* Out-of-range characters all share the one fallback cell. */
    {
        const struct font *f = font_get(FONT_BODY, 0, 0);
        font_rc all = { 0, 0, 40, 40 };
        canvas a = canvas_new(40, 40, 40, 0);
        canvas b = canvas_new(40, 40, 40, 0);
        canvas d = canvas_new(40, 40, 40, 0);

        font_draw_char(&a.s, all, 4, 4, 0x01, f, 0xFFFFFF, -1);
        font_draw_char(&b.s, all, 4, 4, 0xFE, f, 0xFFFFFF, -1);
        font_draw_char(&d.s, all, 4, 4, '\n', f, 0xFFFFFF, -1);
        check(memcmp(a.mem, b.mem, 40 * 40 * sizeof(uint32_t)) == 0,
              "0x01 and 0xFE share the fallback glyph");
        check(memcmp(a.mem, d.mem, 40 * 40 * sizeof(uint32_t)) == 0,
              "newline draws the fallback glyph too");
        check(count_ink(&a, 0) > 0, "the fallback glyph has ink");
        canvas_free(&a);
        canvas_free(&b);
        canvas_free(&d);
    }
}

/* The 8x16 face must still be the original bitmap, pixel for pixel: the
 * whole desktop is drawn with it and a regression there is a regression
 * everywhere. */
static void test_native_face_unchanged(void)
{
    const struct font *f = font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN);
    font_rc all = { 0, 0, 16, 24 };
    int c, x, y, bad = 0;

    for (c = 0x20; c <= 0x7E; c++) {
        canvas cv = canvas_new(16, 24, 16, 0x000000);

        font_draw_char(&cv.s, all, 4, 4, c, f, 0xFFFFFFu, -1);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 8; x++) {
                uint32_t want = (gui_font8x16[c - 0x20][y] & (0x80 >> x))
                                    ? 0xFFFFFFu : 0x000000u;

                if (at(&cv, 4 + x, 4 + y) != want)
                    bad++;
            }
        canvas_free(&cv);
    }
    checkf(bad == 0, "8x16 differs from the source bitmap in %ld places (%ld)",
           (long)bad, 0L);

    /* And it is genuinely 1-bit: no intermediate colours anywhere. */
    {
        canvas cv = canvas_new(200, 20, 200, 0x000000);
        font_rc r = { 0, 0, 200, 20 };
        int mid = 0;

        font_draw_text(&cv.s, r, 0, 2, "Handgloves 0123", f, 0xFFFFFFu, -1);
        for (y = 0; y < 20; y++)
            for (x = 0; x < 200; x++) {
                uint32_t v = at(&cv, x, y);

                if (v != 0 && v != 0xFFFFFFu)
                    mid++;
            }
        checkf(mid == 0, "8x16 antialiased %ld pixels (%ld)", (long)mid, 0L);
        canvas_free(&cv);
    }
}

/* Synthetic bold must add ink, italic must move it sideways, and neither
 * may leak past the run's own line. */
static void test_synthesis(void)
{
    int sz;

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++) {
        const struct font *reg = font_get(sz, FONT_REGULAR, FONT_ROMAN);
        const struct font *bold = font_get(sz, FONT_BOLD, FONT_ROMAN);
        const struct font *ital = font_get(sz, FONT_REGULAR, FONT_ITALIC);
        font_rc all = { 0, 0, 200, 40 };
        canvas a = canvas_new(200, 40, 200, 0);
        canvas b = canvas_new(200, 40, 200, 0);
        canvas c = canvas_new(200, 40, 200, 0);
        int ia, ib, ic;

        font_draw_text(&a.s, all, 10, 4, "Hamburg", reg, 0xFFFFFF, -1);
        font_draw_text(&b.s, all, 10, 4, "Hamburg", bold, 0xFFFFFF, -1);
        font_draw_text(&c.s, all, 10, 4, "Hamburg", ital, 0xFFFFFF, -1);
        ia = count_ink(&a, 0);
        ib = count_ink(&b, 0);
        ic = count_ink(&c, 0);
        checkf(ib > ia * 11 / 10, "size %ld: bold added only %ld px",
               (long)sz, (long)(ib - ia));
        checkf(ib < ia * 25 / 10, "size %ld: bold added too much (%ld px)",
               (long)sz, (long)(ib - ia));
        checkf(ic >= ia * 8 / 10 && ic <= ia * 13 / 10,
               "size %ld: italic changed the ink mass to %ld",
               (long)sz, (long)ic);
        check(memcmp(a.mem, c.mem, 200 * 40 * sizeof(uint32_t)) != 0,
              "italic is not identical to roman");
        canvas_free(&a);
        canvas_free(&b);
        canvas_free(&c);
    }
}

/* Baselines: text of different sizes lines up when the caller subtracts
 * the ascent, which is the whole point of publishing one. */
static void test_baselines(void)
{
    int sz;
    int base = 40;

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++) {
        const struct font *f = font_get(sz, FONT_REGULAR, FONT_ROMAN);
        font_rc all = { 0, 0, 60, 60 };
        canvas cv = canvas_new(60, 60, 60, 0);
        int y, last = -1, first = -1, x;

        font_draw_char(&cv.s, all, 10, base - f->ascent, 'H', f, 0xFFFFFF, -1);
        for (y = 0; y < 60; y++)
            for (x = 0; x < 60; x++)
                if (at(&cv, x, y) != 0) {
                    if (first < 0)
                        first = y;
                    last = y;
                }
        /* 'H' has no descender: its last ink row is the row above the
         * baseline, give or take one row of antialiasing. */
        checkf(last >= base - 2 && last <= base - 1,
               "size %ld: 'H' bottom row %ld", (long)sz, (long)last);
        checkf(first >= base - f->ascent && first <= base - f->cap + 1,
               "size %ld: 'H' top row %ld", (long)sz, (long)first);
        canvas_free(&cv);
    }
}

/* --------------------------------------------------------- proof sheets */

static void write_ppm(const char *dir, const char *name, const canvas *c)
{
    char path[512];
    FILE *fp;
    int x, y;

    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    fp = fopen(path, "wb");
    if (!fp) {
        printf("cannot write %s\n", path);
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", c->s.w, c->s.h);
    for (y = 0; y < c->s.h; y++)
        for (x = 0; x < c->s.w; x++) {
            uint32_t v = at(c, x, y);
            unsigned char rgb[3];

            rgb[0] = (unsigned char)(v >> 16);
            rgb[1] = (unsigned char)(v >> 8);
            rgb[2] = (unsigned char)v;
            fwrite(rgb, 1, 3, fp);
        }
    fclose(fp);
    printf("  %s (%dx%d)\n", path, c->s.w, c->s.h);
}

#define PAPER 0x00FFFFFFu
#define INK   0x00161A1Fu
#define DIM   0x00707A85u
#define LINK  0x001A5FB4u
#define DARK  0x0027333Fu
#define DTEXT 0x00D6DFE8u

static void rule(canvas *c, int y, uint32_t color)
{
    int x;

    for (x = 0; x < c->s.w; x++)
        if (y >= 0 && y < c->s.h)
            c->mem[(long)y * c->stride + x] = color;
}

static void band(canvas *c, int y, int h, uint32_t color)
{
    int i;

    for (i = 0; i < h; i++)
        rule(c, y + i, color);
}

/* Sheet 1: every glyph, at every size, weight and style. */
static void sheet_glyphs(const char *dir)
{
    const int cols = 24;
    const int margin = 12;
    int W = 2 * margin + cols * 20, H = 12;
    int sz, wt, st, i, y;
    canvas c;
    font_rc all;
    const struct font *lbl = font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN);

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++) {
        int rows = (96 + cols - 1) / cols;

        H += 4 * (20 + rows * (size_h[sz] + 4) + 10);
    }
    c = canvas_new(W, H, W, PAPER);
    all = font_mkrc(0, 0, W, H);

    y = 8;
    for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
        for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
            for (st = 0; st < FONT_STYLE_COUNT; st++) {
                const struct font *f = font_get(sz, wt, st);
                int gw = f->w + 4;
                int gh = f->h + 4;

                font_draw_text(&c.s, all, margin, y, f->name, lbl, LINK, -1);
                y += 20;
                for (i = 0; i < 96; i++) {
                    int gx = margin + (i % cols) * gw;
                    int gy = y + (i / cols) * gh;
                    int ch = i < 95 ? 0x20 + i : 0x00;

                    band(&c, gy, 1, 0x00F0F0F0u);
                    if (i < 95)
                        font_draw_char(&c.s, all, gx, gy + 2, ch, f, INK, -1);
                    else
                        font_draw_char(&c.s, all, gx, gy + 2, 0x01, f,
                                       INK, -1);
                }
                y += ((96 + cols - 1) / cols) * gh + 10;
            }
    write_ppm(dir, "font-glyphs", &c);
    canvas_free(&c);
}

/* Sheet 2: a pangram in each of the sixteen faces, light and dark. */
static void sheet_pangrams(const char *dir)
{
    static const char *const line =
        "Sphinx of black quartz, judge my vow! 0123456789 @#$%&";
    int W = 1060, H = 2 * (16 * 8 + 4 * (12 + 16 + 24 + 32)) + 60;
    canvas c = canvas_new(W, H, W, PAPER);
    font_rc all = font_mkrc(0, 0, W, H);
    const struct font *lbl = font_get(FONT_SMALL, FONT_BOLD, FONT_ROMAN);
    int sz, wt, st, y, dark;

    y = 8;
    for (dark = 0; dark < 2; dark++) {
        int top = y;

        if (dark)
            band(&c, top - 6, H - top + 6, DARK);
        for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
            for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
                for (st = 0; st < FONT_STYLE_COUNT; st++) {
                    const struct font *f = font_get(sz, wt, st);

                    font_draw_text(&c.s, all, 12, y + 6, f->name, lbl,
                                   dark ? DIM : DIM, -1);
                    font_draw_text(&c.s, all, 130, y, line, f,
                                   dark ? DTEXT : INK, -1);
                    y += f->h + 8;
                }
        y += 24;
    }
    write_ppm(dir, "font-pangrams", &c);
    canvas_free(&c);
}

/* Greedy word wrap, the way the browser's inline layout will do it. */
static int wrap(canvas *c, font_rc clip, int x, int y, int wide,
                const char *text, const struct font *f, uint32_t fg)
{
    int i = 0;

    while (text[i]) {
        int start = i, last_space = -1, w = 0, end;

        while (text[i] && w + f->w <= wide) {
            if (text[i] == ' ')
                last_space = i;
            w += f->w;
            i++;
        }
        end = i;
        if (text[i] && last_space > start)
            end = last_space;
        font_draw_text_n(&c->s, clip, x, y, text + start, end - start, f,
                         fg, -1);
        y += f->h + 2;
        i = end;
        while (text[i] == ' ')
            i++;
    }
    return y;
}

/* Sheet 3: what the browser will actually ask for -- a page of prose with
 * headings, small print, code and emphasis. */
static void sheet_page(const char *dir)
{
    static const char *const para1 =
        "A typeface is not a picture of letters, it is a set of shapes that "
        "keep working when they are made small, crowded together, and read "
        "quickly by someone who is thinking about something else. That is "
        "why this face was drawn as a grid of pixels rather than scaled "
        "from an outline: at these sizes the grid wins every argument.";
    static const char *const para2 =
        "The larger sizes are re-sampled from the same art. Each glyph is "
        "read as a continuous field, its outline is taken to be the half "
        "coverage contour of that field, and every destination pixel is "
        "area-sampled against it. Straight stems come out crisp, diagonals "
        "come out as clean ramps, and the grey that is left over lands only "
        "where an edge genuinely falls between two pixels.";
    static const char *const items[] = {
        "Body text is the native 8x16 face, unchanged.",
        "Headings are the 12x24 and 16x32 faces, emboldened.",
        "Captions and footnotes use the 6x12 face.",
        "Emphasis is a 1:4 shear applied while blitting."
    };
    const int W = 760;
    int H = 812;
    canvas c = canvas_new(W, H, W, PAPER);
    font_rc all = font_mkrc(0, 0, W, H);
    int x = 40, y = 28, wide = W - 80;
    unsigned k;

    font_draw_text(&c.s, all, x, y, "Reading at four sizes",
                   font_get(FONT_HUGE, FONT_BOLD, FONT_ROMAN), INK, -1);
    y += 42;
    rule(&c, y - 6, 0x00DCDCDCu);
    font_draw_text(&c.s, all, x, y,
                   "Drawn from one 8x16 face, KestrelOS, 2026",
                   font_get(FONT_SMALL, FONT_REGULAR, FONT_ITALIC), DIM, -1);
    y += 26;

    y = wrap(&c, all, x, y, wide, para1,
             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN), INK);
    y += 16;

    font_draw_text(&c.s, all, x, y, "How the larger faces are made",
                   font_get(FONT_LARGE, FONT_BOLD, FONT_ROMAN), INK, -1);
    y += 34;
    y = wrap(&c, all, x, y, wide, para2,
             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN), INK);
    y += 16;

    font_draw_text(&c.s, all, x, y, "What the browser asks for",
                   font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN), INK, -1);
    y += 22;
    for (k = 0; k < sizeof(items) / sizeof(items[0]); k++) {
        font_draw_text(&c.s, all, x + 6, y, "-",
                       font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                       DIM, -1);
        font_draw_text(&c.s, all, x + 22, y, items[k],
                       font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                       INK, -1);
        y += 18;
    }
    y += 14;

    font_draw_text(&c.s, all, x, y, "Emphasis, code and truncation",
                   font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN), INK, -1);
    y += 24;
    {
        int cx = x;

        cx += font_draw_text(&c.s, all, cx, y, "Ordinary text, then ",
                             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                             INK, -1);
        cx += font_draw_text(&c.s, all, cx, y, "italic emphasis",
                             font_get(FONT_BODY, FONT_REGULAR, FONT_ITALIC),
                             INK, -1);
        cx += font_draw_text(&c.s, all, cx, y, ", then ",
                             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                             INK, -1);
        cx += font_draw_text(&c.s, all, cx, y, "strong",
                             font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN),
                             INK, -1);
        font_draw_text(&c.s, all, cx, y, ".",
                       font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                       INK, -1);
        y += 20;
        cx = x;
        cx += font_draw_text(&c.s, all, cx, y, "Code: ",
                             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                             INK, -1);
        font_draw_text(&c.s, all, cx, y, "font_get(FONT_HUGE, FONT_BOLD, 0);",
                       font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                       0x00A03030u, 0x00F2F2F2L);
        y += 22;
    }

    /* Truncation, at three different widths. */
    {
        static const int widths[] = { 320, 180, 90, 40, 20 };
        unsigned i;

        for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
            font_run r;

            band(&c, y - 2, 20, 0x00F6F6F6u);
            font_draw_run(&c.s, all, x, y,
                          "A headline that is far too long for the column",
                          -1, font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN),
                          INK, -1, widths[i], 1, &r);
            font_draw_text(&c.s, all, x + 340, y + 2,
                           r.truncated ? "truncated" : "fits",
                           font_get(FONT_SMALL, FONT_REGULAR, FONT_ITALIC),
                           DIM, -1);
            y += 20;
        }
    }
    y += 10;

    /* Mixed sizes sharing one baseline. */
    {
        int base = y + 32;
        int cx = x;
        static const int mix[] = { FONT_HUGE, FONT_BODY, FONT_LARGE,
                                   FONT_SMALL, FONT_BODY };
        static const char *const word[] = { "Big", "and", "small", "on",
                                            "one line" };
        unsigned i;

        for (i = 0; i < sizeof(mix) / sizeof(mix[0]); i++) {
            const struct font *f = font_get(mix[i], i == 0 ? FONT_BOLD : 0,
                                            0);

            cx += font_draw_text(&c.s, all, cx, base - f->ascent, word[i], f,
                                 INK, -1);
            cx += f->w / 2;
        }
        rule(&c, base, 0x00E0A0A0u);
        y = base + 16;
    }

    /* And the same page furniture on the desktop's dark surface. */
    band(&c, y, H - y, DARK);
    y += 14;
    font_draw_text(&c.s, all, x, y, "On the desktop surface",
                   font_get(FONT_LARGE, FONT_BOLD, FONT_ROMAN), DTEXT, -1);
    y += 32;
    y = wrap(&c, all, x, y, wide,
             "Light text on a dark ground is where antialiasing usually "
             "goes wrong: the grey edge pixels bloom and the letters look "
             "fat. These blend towards the actual background colour, so "
             "they stay the weight they were drawn.",
             font_get(FONT_BODY, FONT_REGULAR, FONT_ROMAN), DTEXT);
    write_ppm(dir, "font-page", &c);
    canvas_free(&c);
}

/* Nearest-neighbour magnification of `src` into `out` at (ox, oy). */
static void blit_zoom(canvas *out, int ox, int oy, const canvas *src,
                      int zoom)
{
    int x, y, a, b;

    for (y = 0; y < src->s.h; y++)
        for (x = 0; x < src->s.w; x++) {
            uint32_t v = at(src, x, y);

            for (a = 0; a < zoom; a++)
                for (b = 0; b < zoom; b++) {
                    int px = ox + x * zoom + b;
                    int py = oy + y * zoom + a;

                    if (px >= 0 && px < out->s.w && py >= 0 && py < out->s.h)
                        out->mem[(long)py * out->stride + px] = v;
                }
        }
}

/* Sheet 4: every size magnified to roughly the same apparent height, so
 * the pixels can actually be judged. */
static void sheet_zoom(const char *dir)
{
    static const char *const rows[] = {
        "Handgloves mint 0123",
        "illegible? 1lI0O8B5S",
        "www.example.com/#m%",
        "The quick brown fox"
    };
    static const int zm[] = { 8, 6, 4, 3 };
    const int W = 1320;
    int sz, i, H = 24;
    canvas out;

    for (sz = 0; sz < FONT_SIZE_COUNT; sz++)
        H += 24 + 4 * (size_h[sz] + 3) * zm[sz];
    out = canvas_new(W, H, 0, PAPER);

    {
        int oy = 12;

        for (sz = 0; sz < FONT_SIZE_COUNT; sz++) {
            int zoom = zm[sz];
            int sh = 4 * (size_h[sz] + 3);
            canvas src = canvas_new(W / zoom, sh, 0, PAPER);
            font_rc all = font_mkrc(0, 0, src.s.w, src.s.h);
            int y = 0;

            for (i = 0; i < 4; i++) {
                const struct font *f =
                    font_get(sz, i == 1 ? FONT_BOLD : FONT_REGULAR,
                             i == 2 ? FONT_ITALIC : FONT_ROMAN);

                font_draw_text(&src.s, all, 2, y, rows[i], f, INK, -1);
                y += size_h[sz] + 3;
            }
            font_draw_text(&out.s, font_mkrc(0, 0, W, H), 8, oy,
                           size_name[sz],
                           font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN),
                           LINK, -1);
            oy += 20;
            blit_zoom(&out, 0, oy, &src, zoom);
            oy += sh * zoom + 4;
            canvas_free(&src);
        }
    }
    write_ppm(dir, "font-zoom", &out);
    canvas_free(&out);
}

/* Sheet 5: the 6x12 face, every glyph, magnified eight times.  This is
 * the size the re-sampler struggles with, so it gets its own sheet and
 * its glyphs are the ones worth hand-correcting. */
static void sheet_small(const char *dir)
{
    const int zoom = 8;
    const int cols = 16;
    const int cell = 8;                    /* 6x12 plus a pixel of margin */
    const int rows = (96 + cols - 1) / cols;
    int wt, st, i;
    int W = cols * cell * zoom + 20;
    int H = 8 + 4 * (20 + rows * 14 * zoom + 10);
    canvas out = canvas_new(W, H, 0, PAPER);
    font_rc page = font_mkrc(0, 0, W, H);
    int oy = 8;

    for (wt = 0; wt < FONT_WEIGHT_COUNT; wt++)
        for (st = 0; st < FONT_STYLE_COUNT; st++) {
            const struct font *f = font_get(FONT_SMALL, wt, st);
            canvas src = canvas_new(cols * cell, rows * 14, 0, PAPER);
            font_rc all = font_mkrc(0, 0, src.s.w, src.s.h);

            for (i = 0; i < 96; i++)
                font_draw_char(&src.s, all, (i % cols) * cell + 1,
                               (i / cols) * 14 + 1,
                               i < 95 ? 0x20 + i : 0x01, f, INK, -1);
            font_draw_text(&out.s, page, 8, oy, f->name,
                           font_get(FONT_BODY, FONT_BOLD, FONT_ROMAN),
                           LINK, -1);
            oy += 20;
            blit_zoom(&out, 8, oy, &src, zoom);
            oy += rows * 14 * zoom + 10;
            canvas_free(&src);
        }
    write_ppm(dir, "font-small", &out);
    canvas_free(&out);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    test_faces();
    test_metrics();
    test_runs();
    test_blend();
    test_clipping();
    test_degenerate_surfaces();
    test_coverage();
    test_native_face_unchanged();
    test_synthesis();
    test_baselines();

    printf("%d checks, %d failures\n", checks, failures);

    if (argc > 1) {
        printf("proof sheets:\n");
        sheet_glyphs(argv[1]);
        sheet_pangrams(argv[1]);
        sheet_page(argv[1]);
        sheet_zoom(argv[1]);
        sheet_small(argv[1]);
    }
    return failures ? 1 : 0;
}
