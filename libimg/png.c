/* KestrelOS libimg: PNG (ISO/IEC 15948).
 *
 * Full baseline: bit depths 1/2/4/8/16, colour types 0/2/3/4/6, all five
 * scanline filters, Adam7 interlacing, PLTE, tRNS, and a CRC-32 check on
 * every chunk. Decompression comes from libz.
 *
 * Structure: one pass over the chunks to validate them and pick up the
 * headers, a second to gather the IDAT payload contiguously, then inflate,
 * unfilter and convert. Nothing recurses; the only large allocations are
 * the IDAT copy, the inflate output and the image itself, all bounded.
 */

#include <stdlib.h>
#include <string.h>

#include "img.h"

/* --------------------------------------------------------------- libz
 *
 * Decompression comes from libz:
 *
 *   int inflate_buf_limit(const void *src, unsigned long slen, void **out,
 *                         unsigned long *olen, int wrapper,
 *                         unsigned long max_out);
 *
 * returning 0 on success with *out malloc()ed and owned by the caller, and
 * a negative value on failure. PNG uses the zlib wrapper, and passes the
 * exact size IHDR implies as the cap: a header claiming an 8x8 image whose
 * IDAT expands to a gigabyte is stopped after a few hundred bytes rather
 * than after the gigabyte. A little slack is allowed so a stream with
 * harmless trailing padding still decodes.
 *
 * Define PNG_NO_INFLATE_HEADER to fall back to the local declarations
 * below, for building this file without libz on the include path.
 */
#ifndef PNG_NO_INFLATE_HEADER
#include "inflate.h"
#endif

#ifndef INFLATE_ZLIB
#define INFLATE_ZLIB 1
#endif

#ifdef PNG_NO_INFLATE_HEADER
int inflate_buf_limit(const void *src, unsigned long slen, void **out,
                      unsigned long *olen, int wrapper,
                      unsigned long max_out);
#endif

/* --------------------------------------------------------------- crc32 */

static uint32_t crc_tab[256];
static int crc_ready;

static void crc_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_of(const uint8_t *p, unsigned long n)
{
    uint32_t c = 0xffffffffu;
    unsigned long i;

    if (!crc_ready)
        crc_init();
    for (i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* --------------------------------------------------------------- reading */

static unsigned long be32(const uint8_t *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* --------------------------------------------------------------- state */

struct png {
    long w, h;
    int  depth, ct, interlace;
    int  channels;          /* samples per pixel */
    int  bpp;               /* bytes per pixel, rounded up, min 1 */
    int  npal;
    uint8_t pal[256 * 3];
    uint8_t pal_a[256];
    int  have_trns;
    unsigned trns[3];       /* raw sample values for colour types 0 and 2 */
    struct image *im;
};

static const int adam_x0[7]   = { 0, 4, 0, 2, 0, 1, 0 };
static const int adam_y0[7]   = { 0, 0, 4, 0, 2, 0, 1 };
static const int adam_dx[7]   = { 8, 8, 4, 4, 2, 2, 1 };
static const int adam_dy[7]   = { 8, 8, 8, 4, 4, 2, 2 };

static int channels_for(int ct)
{
    switch (ct) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

static int depth_ok(int ct, int depth)
{
    switch (ct) {
    case 0: return depth == 1 || depth == 2 || depth == 4 ||
                   depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2: case 4: case 6: return depth == 8 || depth == 16;
    default: return 0;
    }
}

/* Bytes in one scanline of `w` pixels, excluding the filter byte. */
static unsigned long row_stride(const struct png *g, long w)
{
    return ((unsigned long)w * (unsigned)g->channels * (unsigned)g->depth + 7)
           / 8;
}

/* --------------------------------------------------------------- samples */

/* The idx'th sample of a scanline, at the file's native bit depth. */
static unsigned sample_at(const uint8_t *row, int depth, unsigned long idx)
{
    switch (depth) {
    case 1:  return (row[idx >> 3] >> (7 - (idx & 7))) & 1;
    case 2:  return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
    case 4:  return (row[idx >> 1] >> (4 - 4 * (idx & 1))) & 15;
    case 8:  return row[idx];
    default: return ((unsigned)row[idx * 2] << 8) | row[idx * 2 + 1];
    }
}

/* Native sample -> 8 bits, spreading the range so 1-bit 1 becomes 255. */
static int to8(unsigned v, int depth)
{
    switch (depth) {
    case 1:  return (int)(v * 255u);
    case 2:  return (int)(v * 85u);
    case 4:  return (int)(v * 17u);
    case 8:  return (int)v;
    default: return (int)(v >> 8);
    }
}

/* One decoded scanline of `n` pixels into the image, at row `dy`, starting
 * at column `x0` and stepping `dx` (1 for a non-interlaced image). */
static void emit_row(struct png *g, const uint8_t *row, int n, int dy,
                     int x0, int dx)
{
    struct image *im = g->im;
    uint32_t *dst = im->px + (long)dy * im->w;
    uint8_t *da = im->alpha ? im->alpha + (long)dy * im->w : 0;
    int i, x = x0;

    for (i = 0; i < n; i++, x += dx) {
        int r, gr, b, a = 255;

        switch (g->ct) {
        case 0: {
            unsigned s = sample_at(row, g->depth, (unsigned long)i);
            r = gr = b = to8(s, g->depth);
            if (g->have_trns && s == g->trns[0])
                a = 0;
            break;
        }
        case 2: {
            unsigned s0 = sample_at(row, g->depth, (unsigned long)i * 3);
            unsigned s1 = sample_at(row, g->depth, (unsigned long)i * 3 + 1);
            unsigned s2 = sample_at(row, g->depth, (unsigned long)i * 3 + 2);
            r = to8(s0, g->depth);
            gr = to8(s1, g->depth);
            b = to8(s2, g->depth);
            if (g->have_trns && s0 == g->trns[0] && s1 == g->trns[1] &&
                s2 == g->trns[2])
                a = 0;
            break;
        }
        case 3: {
            unsigned s = sample_at(row, g->depth, (unsigned long)i);
            if ((int)s >= g->npal) {
                /* Out of range: the spec forbids it, but rendering black
                 * beats refusing the whole image. */
                r = gr = b = 0;
            } else {
                r = g->pal[s * 3];
                gr = g->pal[s * 3 + 1];
                b = g->pal[s * 3 + 2];
                if (g->have_trns)
                    a = g->pal_a[s];
            }
            break;
        }
        case 4: {
            unsigned s0 = sample_at(row, g->depth, (unsigned long)i * 2);
            unsigned s1 = sample_at(row, g->depth, (unsigned long)i * 2 + 1);
            r = gr = b = to8(s0, g->depth);
            a = to8(s1, g->depth);
            break;
        }
        default: {
            unsigned s0 = sample_at(row, g->depth, (unsigned long)i * 4);
            unsigned s1 = sample_at(row, g->depth, (unsigned long)i * 4 + 1);
            unsigned s2 = sample_at(row, g->depth, (unsigned long)i * 4 + 2);
            unsigned s3 = sample_at(row, g->depth, (unsigned long)i * 4 + 3);
            r = to8(s0, g->depth);
            gr = to8(s1, g->depth);
            b = to8(s2, g->depth);
            a = to8(s3, g->depth);
            break;
        }
        }
        dst[x] = img__rgb(r, gr, b);
        if (da)
            da[x] = (uint8_t)a;
    }
}

/* --------------------------------------------------------------- filters */

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* Reverse one scanline filter in place. `prior` is the already-unfiltered
 * previous line, or a zero line for the first row of a pass. */
static int unfilter(int ft, uint8_t *row, const uint8_t *prior,
                    unsigned long n, int bpp)
{
    unsigned long i;

    switch (ft) {
    case 0:
        break;
    case 1:
        for (i = (unsigned long)bpp; i < n; i++)
            row[i] = (uint8_t)(row[i] + row[i - bpp]);
        break;
    case 2:
        for (i = 0; i < n; i++)
            row[i] = (uint8_t)(row[i] + prior[i]);
        break;
    case 3:
        for (i = 0; i < n; i++) {
            int left = i >= (unsigned long)bpp ? row[i - bpp] : 0;
            row[i] = (uint8_t)(row[i] + ((left + prior[i]) >> 1));
        }
        break;
    case 4:
        for (i = 0; i < n; i++) {
            int left = i >= (unsigned long)bpp ? row[i - bpp] : 0;
            int ul = i >= (unsigned long)bpp ? prior[i - bpp] : 0;
            row[i] = (uint8_t)(row[i] + paeth(left, prior[i], ul));
        }
        break;
    default:
        return IMG_ERR_CORRUPT;
    }
    return IMG_OK;
}

/* --------------------------------------------------------------- passes */

/* Size of the inflated data for one interlace pass (0 if the pass is
 * empty), including one filter byte per scanline. */
static unsigned long pass_bytes(const struct png *g, long pw, long ph)
{
    if (pw <= 0 || ph <= 0)
        return 0;
    return (row_stride(g, pw) + 1) * (unsigned long)ph;
}

static void pass_dims(const struct png *g, int p, long *pw, long *ph)
{
    if (!g->interlace) {
        *pw = g->w;
        *ph = g->h;
        return;
    }
    *pw = (g->w - adam_x0[p] + adam_dx[p] - 1) / adam_dx[p];
    *ph = (g->h - adam_y0[p] + adam_dy[p] - 1) / adam_dy[p];
    if (*pw < 0) *pw = 0;
    if (*ph < 0) *ph = 0;
}

/* Unfilter and emit every pass out of the inflated stream. */
static int decode_passes(struct png *g, uint8_t *raw, unsigned long rawlen)
{
    uint8_t *zero;
    unsigned long pos = 0;
    int p, npass = g->interlace ? 7 : 1;
    int rc = IMG_OK;
    unsigned long maxstride = row_stride(g, g->w) + 1;

    zero = (uint8_t *)calloc(maxstride ? maxstride : 1, 1);
    if (!zero)
        return IMG_ERR_NOMEM;

    for (p = 0; p < npass; p++) {
        long pw, ph, y;
        unsigned long stride;
        const uint8_t *prior = zero;

        pass_dims(g, p, &pw, &ph);
        if (pw <= 0 || ph <= 0)
            continue;
        stride = row_stride(g, pw);

        for (y = 0; y < ph; y++) {
            uint8_t *row;
            int ft;

            if (pos + 1 + stride > rawlen) {
                rc = IMG_ERR_TRUNCATED;
                goto done;
            }
            ft = raw[pos];
            row = raw + pos + 1;
            rc = unfilter(ft, row, prior, stride, g->bpp);
            if (rc != IMG_OK)
                goto done;
            emit_row(g, row, (int)pw,
                     g->interlace ? adam_y0[p] + (int)y * adam_dy[p] : (int)y,
                     g->interlace ? adam_x0[p] : 0,
                     g->interlace ? adam_dx[p] : 1);
            prior = row;
            pos += 1 + stride;
        }
    }
done:
    free(zero);
    return rc;
}

/* --------------------------------------------------------------- decode */

int png_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct png g;
    struct image im;
    unsigned long pos, idat_total = 0, idat_off = 0, expect = 0;
    uint8_t *idat = 0, *raw = 0;
    void *inf = 0;
    unsigned long inflen = 0;
    int rc, seen_ihdr = 0, seen_iend = 0, want_alpha;
    int p;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&g, 0, sizeof(g));
    memset(&im, 0, sizeof(im));
    if (!d || len < 8 || len > IMG_MAX_INPUT)
        return IMG_ERR_TRUNCATED;
    if (img_sniff(d, len) != IMG_FMT_PNG)
        return IMG_ERR_FORMAT;

    /* ---- pass 1: validate chunks, collect headers, measure IDAT ---- */
    pos = 8;
    while (pos + 8 <= len) {
        unsigned long clen = be32(d + pos);
        const uint8_t *type = d + pos + 4;
        const uint8_t *body = d + pos + 8;
        int critical = !(type[0] & 0x20);
        int bad_crc;

        if (clen > 0x7fffffffUL)
            return IMG_ERR_CORRUPT;
        if (clen > len || pos + 12 + clen > len)
            return IMG_ERR_TRUNCATED;

        bad_crc = crc32_of(type, clen + 4) != (uint32_t)be32(body + clen);
        if (bad_crc && critical)
            return IMG_ERR_CORRUPT;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (seen_ihdr || clen != 13)
                return IMG_ERR_CORRUPT;
            seen_ihdr = 1;
            g.w = (long)be32(body);
            g.h = (long)be32(body + 4);
            g.depth = body[8];
            g.ct = body[9];
            g.interlace = body[12];
            if (be32(body) > 0x7fffffffUL || be32(body + 4) > 0x7fffffffUL)
                return IMG_ERR_CORRUPT;
            if (g.w == 0 || g.h == 0)
                return IMG_ERR_CORRUPT;
            if (body[10] != 0 || body[11] != 0)
                return IMG_ERR_UNSUPPORTED;   /* only method 0 exists */
            if (g.interlace > 1)
                return IMG_ERR_UNSUPPORTED;
            if (!depth_ok(g.ct, g.depth))
                return IMG_ERR_CORRUPT;
            if (g.w > IMG_MAX_DIM || g.h > IMG_MAX_DIM ||
                (unsigned long)g.w * (unsigned long)g.h > IMG_MAX_PIXELS)
                return IMG_ERR_TOOBIG;
            g.channels = channels_for(g.ct);
            g.bpp = (g.channels * g.depth + 7) / 8;
            if (g.bpp < 1)
                g.bpp = 1;
        } else if (!seen_ihdr) {
            return IMG_ERR_CORRUPT;           /* IHDR must come first */
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (clen == 0 || clen > 768 || clen % 3)
                return IMG_ERR_CORRUPT;
            if (g.ct == 0 || g.ct == 4)
                return IMG_ERR_CORRUPT;       /* meaningless for greyscale */
            g.npal = (int)(clen / 3);
            memcpy(g.pal, body, clen);
            memset(g.pal_a, 0xff, sizeof(g.pal_a));
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (bad_crc)
                goto next;                    /* ancillary: just drop it */
            if (g.ct == 3) {
                if (clen > 256)
                    return IMG_ERR_CORRUPT;
                memset(g.pal_a, 0xff, sizeof(g.pal_a));
                memcpy(g.pal_a, body, clen);
                g.have_trns = 1;
            } else if (g.ct == 0) {
                if (clen != 2)
                    return IMG_ERR_CORRUPT;
                g.trns[0] = ((unsigned)body[0] << 8) | body[1];
                if (g.depth < 16)
                    g.trns[0] &= (1u << g.depth) - 1;
                g.have_trns = 1;
            } else if (g.ct == 2) {
                if (clen != 6)
                    return IMG_ERR_CORRUPT;
                for (p = 0; p < 3; p++) {
                    g.trns[p] = ((unsigned)body[p * 2] << 8) | body[p * 2 + 1];
                    if (g.depth < 16)
                        g.trns[p] &= (1u << g.depth) - 1;
                }
                g.have_trns = 1;
            }
            /* tRNS on colour types 4 and 6 is invalid; ignore it. */
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_total + clen < idat_total)
                return IMG_ERR_TOOBIG;
            idat_total += clen;
            if (idat_total > IMG_MAX_INPUT)
                return IMG_ERR_TOOBIG;
        } else if (memcmp(type, "IEND", 4) == 0) {
            seen_iend = 1;
            pos += 12 + clen;
            break;
        }
next:
        pos += 12 + clen;
    }

    if (!seen_ihdr)
        return IMG_ERR_CORRUPT;
    if (g.ct == 3 && g.npal == 0)
        return IMG_ERR_CORRUPT;               /* PLTE is mandatory here */
    if (idat_total == 0)
        return seen_iend ? IMG_ERR_CORRUPT : IMG_ERR_TRUNCATED;

    /* Exact size of the filtered image data implied by the header. */
    if (g.interlace) {
        for (p = 0; p < 7; p++) {
            long pw, ph;
            pass_dims(&g, p, &pw, &ph);
            expect += pass_bytes(&g, pw, ph);
        }
    } else {
        expect = pass_bytes(&g, g.w, g.h);
    }
    if (expect == 0 || expect > IMG_MAX_RAW)
        return IMG_ERR_TOOBIG;

    /* ---- pass 2: gather IDAT contiguously ---- */
    idat = (uint8_t *)malloc(idat_total);
    if (!idat)
        return IMG_ERR_NOMEM;
    pos = 8;
    while (pos + 12 <= len) {
        unsigned long clen = be32(d + pos);
        if (clen > len || pos + 12 + clen > len)
            break;
        if (memcmp(d + pos + 4, "IDAT", 4) == 0) {
            if (idat_off + clen > idat_total)
                break;
            memcpy(idat + idat_off, d + pos + 8, clen);
            idat_off += clen;
        } else if (memcmp(d + pos + 4, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }

    /* ---- inflate ---- */
    rc = inflate_buf_limit(idat, idat_off, &inf, &inflen, INFLATE_ZLIB,
                           expect + 4096);
    free(idat);
    idat = 0;
    if (rc != 0 || !inf)
        return IMG_ERR_CORRUPT;

    /* A short stream is a truncated download, which is worth rendering as
     * far as it goes; a long one means the header lied, and we use only
     * what the header called for. */
    raw = (uint8_t *)malloc(expect);
    if (!raw) {
        free(inf);
        return IMG_ERR_NOMEM;
    }
    memset(raw, 0, expect);
    memcpy(raw, inf, inflen < expect ? inflen : expect);
    free(inf);

    /* ---- convert ---- */
    want_alpha = (g.ct == 4 || g.ct == 6 || g.have_trns);
    rc = img__alloc(&im, g.w, g.h, want_alpha);
    if (rc != IMG_OK) {
        free(raw);
        return rc;
    }
    g.im = &im;
    rc = decode_passes(&g, raw, expect);
    free(raw);
    if (rc != IMG_OK) {
        img_free(&im);
        return rc;
    }
    *out = im;
    return IMG_OK;
}
