/* KestrelOS libimg: GIF87a / GIF89a.
 *
 * Decodes the FIRST frame onto the logical screen and reports how many
 * frames the file contains. Animation is deliberately out of scope: a
 * still frame plus an honest count beats a fake.
 *
 * The LZW decoder is written from the spec and handles what real encoders
 * actually emit, including the deferred clear code -- once the table is
 * full, well-behaved encoders send a clear, but plenty do not and simply
 * keep coding at 12 bits against the frozen table.
 */

#include <stdlib.h>
#include <string.h>

#include "img.h"

#define GIF_MAX_CODES 4096

struct lzw {
    uint16_t prefix[GIF_MAX_CODES];
    uint8_t  suffix[GIF_MAX_CODES];
    uint8_t  stack[GIF_MAX_CODES];
};

static unsigned rd16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }

/* --------------------------------------------------------------- blocks */

/* Walk past a chain of data sub-blocks. *pos points at the first length
 * byte on entry and just past the terminator on success. */
static int skip_blocks(const uint8_t *d, unsigned long len, unsigned long *pos)
{
    while (*pos < len) {
        unsigned long n = d[*pos];
        if (n == 0) {
            (*pos)++;
            return 0;
        }
        if (*pos + 1 + n > len)
            return -1;
        *pos += 1 + n;
    }
    return -1;
}

/* --------------------------------------------------------------- LZW
 *
 * Codes arrive least-significant bit first, as one continuous stream
 * spread across the data sub-blocks.
 */
struct bits {
    const uint8_t *d;
    unsigned long len;
    unsigned long left;     /* bytes remaining in the current sub-block */
    unsigned long cur;      /* next byte to read */
    unsigned long acc;
    int nbits;
};

static void bits_init(struct bits *b, const uint8_t *d, unsigned long len,
                      unsigned long pos)
{
    b->d = d;
    b->len = len;
    b->left = 0;
    b->cur = pos;
    b->acc = 0;
    b->nbits = 0;
}

static int bits_byte(struct bits *b)
{
    while (b->left == 0) {
        unsigned long n;

        if (b->cur >= b->len)
            return -1;
        n = b->d[b->cur];
        if (n == 0)
            return -1;                    /* block terminator: stream over */
        if (b->cur + 1 + n > b->len) {
            /* Truncated last sub-block: consume whatever is really there. */
            n = b->len - b->cur - 1;
            if (n == 0)
                return -1;
        }
        b->left = n;
        b->cur++;
    }
    b->left--;
    return b->d[b->cur++];
}

static int bits_get(struct bits *b, int n)
{
    int v;

    while (b->nbits < n) {
        int c = bits_byte(b);
        if (c < 0)
            return -1;
        b->acc |= (unsigned long)c << b->nbits;
        b->nbits += 8;
    }
    v = (int)(b->acc & ((1UL << n) - 1));
    b->acc >>= n;
    b->nbits -= n;
    return v;
}

static int lzw_decode(struct lzw *t, struct bits *b, int min_code_size,
                      uint8_t *out, unsigned long npix)
{
    int clear = 1 << min_code_size;
    int end = clear + 1;
    int width = min_code_size + 1;
    int next = clear + 2;
    int prev = -1;
    int first = 0;
    unsigned long n = 0;

    for (;;) {
        int code = bits_get(b, width);
        int sp = 0;
        int c;

        if (code < 0)
            break;                        /* out of data: keep what we have */
        if (code == clear) {
            width = min_code_size + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == end)
            break;

        if (code < next) {
            c = code;
        } else if (code == next && prev >= 0) {
            /* KwKwK: the entry being defined is referenced immediately. */
            t->stack[sp++] = (uint8_t)first;
            c = prev;
        } else {
            break;                        /* corrupt: stop, keep the prefix */
        }

        /* Walk the chain to its root. Bounded by the table size, so a
         * corrupt table containing a cycle terminates rather than hangs. */
        while (c >= clear + 2) {
            if (sp >= GIF_MAX_CODES || c >= GIF_MAX_CODES)
                return IMG_ERR_CORRUPT;
            t->stack[sp++] = t->suffix[c];
            c = t->prefix[c];
        }
        if (sp >= GIF_MAX_CODES)
            return IMG_ERR_CORRUPT;
        if (c > 255)
            c = 0;                        /* a clear/end code inside a chain */
        t->stack[sp++] = (uint8_t)c;
        first = c;

        while (sp > 0 && n < npix)
            out[n++] = t->stack[--sp];

        if (prev >= 0 && next < GIF_MAX_CODES) {
            t->prefix[next] = (uint16_t)prev;
            t->suffix[next] = (uint8_t)first;
            next++;
            /* Widen exactly when the next code can no longer be expressed.
             * Once next hits 4096 the table freezes and the width stays at
             * 12 -- the deferred clear that real encoders rely on. */
            if (next < GIF_MAX_CODES && (next & (next - 1)) == 0 && width < 12)
                width++;
        }
        prev = code;

        if (n >= npix)
            break;
    }
    return IMG_OK;
}

/* --------------------------------------------------------------- painting */

static const int ilace_y0[4] = { 0, 4, 2, 1 };
static const int ilace_dy[4] = { 8, 8, 4, 2 };

struct frame {
    long fx, fy, fw, fh;
    long scr_w, scr_h;
    int  npal, transparent;
    const uint8_t *pal;
};

static void paint_row(struct image *im, const struct frame *f,
                      const uint8_t *src, long dy)
{
    long x;

    if (dy < 0 || dy >= f->scr_h)
        return;
    for (x = 0; x < f->fw; x++) {
        long dx = f->fx + x;
        int ci = src[x];

        if (dx < 0 || dx >= f->scr_w)
            continue;
        if (ci >= f->npal)
            ci = 0;
        /* A transparent pixel still gets its palette colour: the coverage
         * plane says it is invisible, and leaving a defined colour behind
         * means nothing downstream ever reads uninitialised junk. */
        im->px[dy * f->scr_w + dx] = img__rgb(f->pal[ci * 3],
                                              f->pal[ci * 3 + 1],
                                              f->pal[ci * 3 + 2]);
        if (im->alpha)
            im->alpha[dy * f->scr_w + dx] =
                (f->transparent >= 0 && ci == f->transparent) ? 0 : 255;
    }
}

/* --------------------------------------------------------------- decode */

int gif_decode(const void *data, unsigned long len, struct image *out,
               int *frames)
{
    const uint8_t *d = (const uint8_t *)data;
    struct image im;
    struct frame f;
    struct lzw *tab = 0;
    uint8_t *idx = 0;
    uint8_t gct[256 * 3], lct[256 * 3];
    int ngct = 0;
    int transparent = -1;
    unsigned long pos;
    long scr_w, scr_h;
    int rc = IMG_OK, got_frame = 0, nframes = 0;

    if (frames)
        *frames = 0;
    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));
    memset(&f, 0, sizeof(f));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;
    if (len < 13)
        return IMG_ERR_TRUNCATED;
    if (img_sniff(d, len) != IMG_FMT_GIF)
        return IMG_ERR_FORMAT;

    scr_w = (long)rd16(d + 6);
    scr_h = (long)rd16(d + 8);
    pos = 13;
    if (d[10] & 0x80) {
        ngct = 1 << ((d[10] & 7) + 1);
        if (pos + (unsigned long)ngct * 3 > len)
            return IMG_ERR_TRUNCATED;
        memcpy(gct, d + pos, (unsigned long)ngct * 3);
        pos += (unsigned long)ngct * 3;
    }

    while (pos < len) {
        uint8_t blk = d[pos++];

        if (blk == 0x3b)                   /* trailer */
            break;

        if (blk == 0x21) {                 /* extension */
            int label;

            if (pos >= len)
                break;
            label = d[pos++];
            if (label == 0xf9 && !got_frame && pos + 6 <= len && d[pos] == 4) {
                if (d[pos + 1] & 1)
                    transparent = d[pos + 4];
            }
            if (skip_blocks(d, len, &pos) != 0)
                break;
            continue;
        }

        if (blk != 0x2c)                   /* junk: stop scanning */
            break;

        /* ---- image descriptor ---- */
        if (pos + 9 > len)
            break;
        {
            long fx = (long)rd16(d + pos);
            long fy = (long)rd16(d + pos + 2);
            long fw = (long)rd16(d + pos + 4);
            long fh = (long)rd16(d + pos + 6);
            int packed = d[pos + 8];
            int interlaced = (packed & 0x40) != 0;
            const uint8_t *pal;
            unsigned long data_start;
            int npal, mcs;
            struct bits b;

            pos += 9;
            nframes++;

            if (packed & 0x80) {
                int n = 1 << ((packed & 7) + 1);
                if (pos + (unsigned long)n * 3 > len)
                    break;
                memcpy(lct, d + pos, (unsigned long)n * 3);
                pos += (unsigned long)n * 3;
                npal = n;
                pal = lct;
            } else {
                npal = ngct;
                pal = gct;
            }

            if (pos >= len)
                break;
            mcs = d[pos++];
            data_start = pos;

            if (got_frame) {               /* counting only from here on */
                if (skip_blocks(d, len, &pos) != 0)
                    break;
                continue;
            }

            if (mcs < 1 || mcs > 8 || npal == 0 || fw <= 0 || fh <= 0) {
                rc = IMG_ERR_CORRUPT;
                goto fail;
            }

            /* A zero (or too small) logical screen turns up in hand-made
             * files; grow the canvas so the frame still fits. */
            if (scr_w < fx + fw)
                scr_w = fx + fw;
            if (scr_h < fy + fh)
                scr_h = fy + fh;
            if (scr_w > IMG_MAX_DIM || scr_h > IMG_MAX_DIM ||
                (unsigned long)scr_w * (unsigned long)scr_h > IMG_MAX_PIXELS) {
                rc = IMG_ERR_TOOBIG;
                goto fail;
            }

            /* Anything the first frame does not paint stays transparent,
             * which is what browsers show for a partial first frame. */
            rc = img__alloc(&im, scr_w, scr_h,
                            transparent >= 0 || fx > 0 || fy > 0 ||
                            fx + fw < scr_w || fy + fh < scr_h);
            if (rc != IMG_OK)
                goto fail;
            /* Anything the frame does not paint reads as transparent
             * black, which is what a browser composites a partial first
             * frame against. */
            if (im.alpha)
                memset(im.alpha, 0, (unsigned long)scr_w * scr_h);

            tab = (struct lzw *)malloc(sizeof(struct lzw));
            idx = (uint8_t *)malloc((unsigned long)fw * (unsigned long)fh);
            if (!tab || !idx) {
                rc = IMG_ERR_NOMEM;
                goto fail;
            }
            memset(tab, 0, sizeof(*tab));
            memset(idx, 0, (unsigned long)fw * (unsigned long)fh);

            bits_init(&b, d, len, data_start);
            rc = lzw_decode(tab, &b, mcs, idx, (unsigned long)fw * fh);
            if (rc != IMG_OK)
                goto fail;

            f.fx = fx; f.fy = fy; f.fw = fw; f.fh = fh;
            f.scr_w = scr_w; f.scr_h = scr_h;
            f.npal = npal; f.transparent = transparent; f.pal = pal;

            if (interlaced) {
                long src = 0, dy;
                int p;
                for (p = 0; p < 4; p++)
                    for (dy = ilace_y0[p]; dy < fh; dy += ilace_dy[p])
                        paint_row(&im, &f, idx + src++ * fw, fy + dy);
            } else {
                long dy;
                for (dy = 0; dy < fh; dy++)
                    paint_row(&im, &f, idx + dy * fw, fy + dy);
            }

            free(tab); tab = 0;
            free(idx); idx = 0;
            got_frame = 1;

            pos = data_start;
            if (skip_blocks(d, len, &pos) != 0)
                break;                     /* truncated: stop counting */
        }
    }

    if (!got_frame) {
        rc = IMG_ERR_TRUNCATED;
        goto fail;
    }
    if (frames)
        *frames = nframes;
    *out = im;
    return IMG_OK;

fail:
    if (tab) free(tab);
    if (idx) free(idx);
    img_free(&im);
    if (frames)
        *frames = nframes;
    return rc;
}
