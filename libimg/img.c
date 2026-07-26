/* KestrelOS libimg: format dispatch, allocation policy and scaling. */

#include <stdlib.h>
#include <string.h>

#include "img.h"

/* ---------------------------------------------------------------- status */

const char *img_error(int code)
{
    switch (code) {
    case IMG_OK:              return "ok";
    case IMG_ERR_FORMAT:      return "not a recognised image format";
    case IMG_ERR_TRUNCATED:   return "truncated image";
    case IMG_ERR_CORRUPT:     return "corrupt image";
    case IMG_ERR_UNSUPPORTED: return "unsupported image variant";
    case IMG_ERR_TOOBIG:      return "image exceeds size limits";
    case IMG_ERR_NOMEM:       return "out of memory";
    case IMG_ERR_INVAL:       return "invalid argument";
    case IMG_ERR_PROGRESSIVE: return "progressive JPEG not supported";
    default:                  return "unknown error";
    }
}

const char *img_format_name(int fmt)
{
    switch (fmt) {
    case IMG_FMT_PNG:  return "PNG";
    case IMG_FMT_GIF:  return "GIF";
    case IMG_FMT_JPEG: return "JPEG";
    case IMG_FMT_BMP:  return "BMP";
    default:           return "unknown";
    }
}

/* ---------------------------------------------------------------- sniffing */

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

int img_sniff(const void *data, unsigned long len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (!p)
        return IMG_FMT_UNKNOWN;
    if (len >= 8 && memcmp(p, png_sig, 8) == 0)
        return IMG_FMT_PNG;
    if (len >= 6 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8' &&
        (p[4] == '7' || p[4] == '9') && p[5] == 'a')
        return IMG_FMT_GIF;
    /* SOI followed by any marker. Real files always have an APPn or COM
     * next; requiring only the 0xff keeps odd-but-valid files working. */
    if (len >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
        return IMG_FMT_JPEG;
    if (len >= 2 && p[0] == 'B' && p[1] == 'M')
        return IMG_FMT_BMP;
    return IMG_FMT_UNKNOWN;
}

/* ---------------------------------------------------------------- memory */

void img_free(struct image *im)
{
    if (!im)
        return;
    if (im->px)
        free(im->px);
    if (im->alpha)
        free(im->alpha);
    im->px = 0;
    im->alpha = 0;
    im->w = im->h = 0;
    im->has_alpha = 0;
}

int img__alloc(struct image *im, long w, long h, int want_alpha)
{
    unsigned long n;

    memset(im, 0, sizeof(*im));
    if (w <= 0 || h <= 0)
        return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return IMG_ERR_TOOBIG;
    n = (unsigned long)w * (unsigned long)h;
    if (n > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;

    im->px = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!im->px)
        return IMG_ERR_NOMEM;
    if (want_alpha) {
        im->alpha = (uint8_t *)malloc(n);
        if (!im->alpha) {
            free(im->px);
            memset(im, 0, sizeof(*im));
            return IMG_ERR_NOMEM;
        }
        memset(im->alpha, 0xff, n);
        im->has_alpha = 1;
    }
    im->w = (int)w;
    im->h = (int)h;
    return IMG_OK;
}

/* ---------------------------------------------------------------- decode */

int img_decode(const void *data, unsigned long len, struct image *out)
{
    int fmt;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (!data)
        return IMG_ERR_INVAL;
    if (len > IMG_MAX_INPUT)
        return IMG_ERR_TOOBIG;

    fmt = img_sniff(data, len);
    switch (fmt) {
    case IMG_FMT_PNG:  return png_decode(data, len, out);
    case IMG_FMT_GIF:  return gif_decode(data, len, out, 0);
    case IMG_FMT_JPEG: return jpeg_decode(data, len, out);
    case IMG_FMT_BMP:  return bmp_decode(data, len, out);
    default:           return IMG_ERR_FORMAT;
    }
}

/* ---------------------------------------------------------------- probe */

static unsigned long rd32be(const uint8_t *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}
static unsigned rd16le(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd16be(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned long rd32le(const uint8_t *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int probe_png(const uint8_t *p, unsigned long len, struct img_info *o)
{
    unsigned long pos = 8;

    /* IHDR must be the first chunk. Walk on to spot tRNS/alpha types. */
    if (len < 8 + 8 + 13)
        return IMG_ERR_TRUNCATED;
    if (memcmp(p + 12, "IHDR", 4) != 0 || rd32be(p + 8) != 13)
        return IMG_ERR_CORRUPT;
    o->w = (int)rd32be(p + 16);
    o->h = (int)rd32be(p + 20);
    if (rd32be(p + 16) > 0x7fffffffUL || rd32be(p + 20) > 0x7fffffffUL)
        return IMG_ERR_CORRUPT;
    o->has_alpha = (p[25] == 4 || p[25] == 6);

    while (pos + 8 <= len) {
        unsigned long clen = rd32be(p + pos);
        if (clen > 0x7fffffffUL || pos + 12 + clen < pos || pos + 12 + clen > len)
            break;
        if (memcmp(p + pos + 4, "tRNS", 4) == 0)
            o->has_alpha = 1;
        if (memcmp(p + pos + 4, "IDAT", 4) == 0 ||
            memcmp(p + pos + 4, "IEND", 4) == 0)
            break;
        pos += 12 + clen;
    }
    return IMG_OK;
}

static int probe_gif(const uint8_t *p, unsigned long len, struct img_info *o)
{
    unsigned long pos;
    int gct;

    if (len < 13)
        return IMG_ERR_TRUNCATED;
    o->w = (int)rd16le(p + 6);
    o->h = (int)rd16le(p + 8);
    gct = (p[10] & 0x80) ? (3 << ((p[10] & 7) + 1)) : 0;
    pos = 13 + (unsigned long)gct;
    o->frames = 0;

    while (pos < len) {
        uint8_t b = p[pos++];
        if (b == 0x3b)
            break;
        if (b == 0x21) {                      /* extension */
            if (pos >= len)
                break;
            if (p[pos] == 0xf9)
                o->has_alpha = 1;             /* a GCE may carry transparency */
            pos++;
            while (pos < len && p[pos]) {     /* sub-blocks */
                unsigned long sz = p[pos];
                pos += sz + 1;
            }
            pos++;
        } else if (b == 0x2c) {               /* image descriptor */
            if (pos + 9 > len)
                break;
            if (o->frames == 0 && (o->w == 0 || o->h == 0)) {
                o->w = (int)rd16le(p + pos + 4);
                o->h = (int)rd16le(p + pos + 6);
            }
            o->frames++;
            {
                int lct = (p[pos + 8] & 0x80) ? (3 << ((p[pos + 8] & 7) + 1)) : 0;
                pos += 9 + (unsigned long)lct;
            }
            if (pos >= len)
                break;
            pos++;                            /* LZW minimum code size */
            while (pos < len && p[pos]) {
                unsigned long sz = p[pos];
                pos += sz + 1;
            }
            pos++;
        } else {
            break;                            /* junk: stop counting */
        }
    }
    if (o->frames < 1)
        o->frames = 1;
    return IMG_OK;
}

static int probe_jpeg(const uint8_t *p, unsigned long len, struct img_info *o)
{
    unsigned long pos = 2;

    while (pos + 4 <= len) {
        unsigned m, seg;
        if (p[pos] != 0xff) { pos++; continue; }
        while (pos < len && p[pos] == 0xff)
            pos++;
        if (pos >= len)
            break;
        m = p[pos++];
        if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7))
            continue;
        if (m == 0xd9)
            break;
        if (pos + 2 > len)
            break;
        seg = rd16be(p + pos);
        if (seg < 2 || pos + seg > len)
            break;
        /* Any SOFn but DAC (0xc4/0xc8/0xcc are DHT/JPG/DAC). */
        if ((m >= 0xc0 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            if (seg < 8)
                return IMG_ERR_CORRUPT;
            o->h = (int)rd16be(p + pos + 3);
            o->w = (int)rd16be(p + pos + 5);
            return IMG_OK;
        }
        if (m == 0xda)
            break;
        pos += seg;
    }
    return IMG_ERR_CORRUPT;
}

static int probe_bmp(const uint8_t *p, unsigned long len, struct img_info *o)
{
    long h;

    if (len < 26)
        return IMG_ERR_TRUNCATED;
    if (rd32le(p + 14) < 12)                  /* DIB header size */
        return IMG_ERR_CORRUPT;
    o->w = (int)(int32_t)(uint32_t)rd32le(p + 18);
    h    = (long)(int32_t)(uint32_t)rd32le(p + 22);
    o->h = (int)(h < 0 ? -h : h);
    return IMG_OK;
}

int img_probe(const void *data, unsigned long len, struct img_info *out)
{
    const uint8_t *p = (const uint8_t *)data;
    int r;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->frames = 1;
    if (!p || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    out->fmt = img_sniff(p, len);
    switch (out->fmt) {
    case IMG_FMT_PNG:  r = probe_png(p, len, out);  break;
    case IMG_FMT_GIF:  r = probe_gif(p, len, out);  break;
    case IMG_FMT_JPEG: r = probe_jpeg(p, len, out); break;
    case IMG_FMT_BMP:  r = probe_bmp(p, len, out);  break;
    default:           return IMG_ERR_FORMAT;
    }
    if (r != IMG_OK)
        return r;
    if (out->w <= 0 || out->h <= 0)
        return IMG_ERR_CORRUPT;
    if (out->w > IMG_MAX_DIM || out->h > IMG_MAX_DIM ||
        (unsigned long)out->w * (unsigned long)out->h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;
    if (out->frames < 1)
        out->frames = 1;
    return IMG_OK;
}

/* ---------------------------------------------------------------- scaling
 *
 * Separable: one pass along each axis, each pass independently choosing an
 * area-average when that axis shrinks and bilinear when it grows. The pass
 * order is chosen so the intermediate buffer is the smaller of the two
 * possibilities.
 *
 * Colour is filtered alpha-weighted (equivalent to filtering premultiplied
 * colour and dividing the result back out), so a fully transparent pixel
 * contributes coverage but not colour. Without that, a transparent black
 * border darkens everything it touches when the image is scaled.
 */

/* One 1-D resample of `sn` samples to `dn`, stepping by `sstep`/`dstep`
 * elements. Used for both a row (step 1) and a column (step = width). */
static void resample(const uint32_t *sp, const uint8_t *sa, int sn, int sstep,
                     uint32_t *dp, uint8_t *da, int dn, int dstep)
{
    int i, j;

    if (dn <= sn) {
        /* Area average. Work in units of 1/dn of a source pixel: destination
         * pixel i covers [i*sn, (i+1)*sn) and source pixel j covers
         * [j*dn, (j+1)*dn), so the weights sum to exactly sn. */
        for (i = 0; i < dn; i++) {
            long long lo = (long long)i * sn;
            long long hi = lo + sn;
            int j0 = (int)(lo / dn);
            int j1 = (int)((hi + dn - 1) / dn);
            unsigned long long ar = 0, ag = 0, ab = 0, aa = 0;

            if (j1 > sn)
                j1 = sn;
            for (j = j0; j < j1; j++) {
                long long s = (long long)j * dn;
                long long e = s + dn;
                long long lw = (e < hi ? e : hi) - (s > lo ? s : lo);
                unsigned long long w;
                uint32_t c;
                unsigned r, g, b;

                if (lw <= 0)
                    continue;
                w = (unsigned long long)lw;
                c = sp[(long)j * sstep];
                r = (c >> 16) & 0xff;
                g = (c >> 8) & 0xff;
                b = c & 0xff;
                if (sa) {
                    unsigned long long a = sa[(long)j * sstep];
                    aa += w * a;
                    w *= a;
                }
                ar += w * r;
                ag += w * g;
                ab += w * b;
            }
            if (sa) {
                unsigned long long tot = aa;
                da[(long)i * dstep] =
                    (uint8_t)((aa + (unsigned)sn / 2) / (unsigned)sn);
                if (tot == 0)
                    dp[(long)i * dstep] = 0;
                else
                    dp[(long)i * dstep] = img__rgb((int)((ar + tot / 2) / tot),
                                                   (int)((ag + tot / 2) / tot),
                                                   (int)((ab + tot / 2) / tot));
            } else {
                unsigned long long tot = (unsigned long long)sn;
                dp[(long)i * dstep] = img__rgb((int)((ar + tot / 2) / tot),
                                               (int)((ag + tot / 2) / tot),
                                               (int)((ab + tot / 2) / tot));
            }
        }
        return;
    }

    /* Bilinear. Destination pixel centre i+0.5 maps to source coordinate
     * (i+0.5)*sn/dn - 0.5, held as 16.16 and clamped to the source edges. */
    for (i = 0; i < dn; i++) {
        long long pos = ((long long)(2 * i + 1) * sn * 32768) / dn - 32768;
        long long maxp = (long long)(sn - 1) << 16;
        int j0, j1, f;
        uint32_t c0, c1;
        unsigned r0, g0, b0, r1, g1, b1;

        if (pos < 0)
            pos = 0;
        if (pos > maxp)
            pos = maxp;
        j0 = (int)(pos >> 16);
        f = (int)(pos & 0xffff);
        j1 = (j0 + 1 < sn) ? j0 + 1 : j0;

        c0 = sp[(long)j0 * sstep];
        c1 = sp[(long)j1 * sstep];
        r0 = (c0 >> 16) & 0xff; g0 = (c0 >> 8) & 0xff; b0 = c0 & 0xff;
        r1 = (c1 >> 16) & 0xff; g1 = (c1 >> 8) & 0xff; b1 = c1 & 0xff;

        if (sa) {
            unsigned long long a0 = sa[(long)j0 * sstep];
            unsigned long long a1 = sa[(long)j1 * sstep];
            unsigned long long w0 = a0 * (unsigned)(65536 - f);
            unsigned long long w1 = a1 * (unsigned)f;
            unsigned long long tot = w0 + w1;

            da[(long)i * dstep] =
                (uint8_t)((a0 * (unsigned)(65536 - f) + a1 * (unsigned)f +
                           32768) >> 16);
            if (tot == 0)
                dp[(long)i * dstep] = 0;
            else
                dp[(long)i * dstep] =
                    img__rgb((int)((w0 * r0 + w1 * r1 + tot / 2) / tot),
                             (int)((w0 * g0 + w1 * g1 + tot / 2) / tot),
                             (int)((w0 * b0 + w1 * b1 + tot / 2) / tot));
        } else {
            unsigned uf = (unsigned)f;
            dp[(long)i * dstep] =
                img__rgb((int)((r0 * (65536 - uf) + r1 * uf + 32768) >> 16),
                         (int)((g0 * (65536 - uf) + g1 * uf + 32768) >> 16),
                         (int)((b0 * (65536 - uf) + b1 * uf + 32768) >> 16));
        }
    }
}

int img_scale(const struct image *src, int w, int h, struct image *out)
{
    struct image tmp;
    const uint8_t *sa;
    int rc, y, x, horiz_first;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (!src || !src->px || src->w <= 0 || src->h <= 0)
        return IMG_ERR_INVAL;
    if (src->has_alpha && !src->alpha)
        return IMG_ERR_INVAL;
    if (w <= 0 || h <= 0)
        return IMG_ERR_INVAL;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM ||
        (unsigned long)w * (unsigned long)h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;

    sa = src->has_alpha ? src->alpha : 0;

    if (w == src->w && h == src->h) {         /* nothing to do but copy */
        rc = img__alloc(out, w, h, src->has_alpha);
        if (rc != IMG_OK)
            return rc;
        memcpy(out->px, src->px, (unsigned long)w * h * sizeof(uint32_t));
        if (src->has_alpha)
            memcpy(out->alpha, src->alpha, (unsigned long)w * h);
        return IMG_OK;
    }

    /* Pick the pass order with the smaller intermediate. */
    horiz_first = ((unsigned long)w * src->h <= (unsigned long)src->w * h);

    if (horiz_first) {
        rc = img__alloc(&tmp, w, src->h, src->has_alpha);
        if (rc != IMG_OK)
            return rc;
        for (y = 0; y < src->h; y++)
            resample(src->px + (long)y * src->w,
                     sa ? sa + (long)y * src->w : 0, src->w, 1,
                     tmp.px + (long)y * w,
                     tmp.alpha ? tmp.alpha + (long)y * w : 0, w, 1);
        rc = img__alloc(out, w, h, src->has_alpha);
        if (rc != IMG_OK) { img_free(&tmp); return rc; }
        for (x = 0; x < w; x++)
            resample(tmp.px + x, tmp.alpha ? tmp.alpha + x : 0, src->h, w,
                     out->px + x, out->alpha ? out->alpha + x : 0, h, w);
    } else {
        rc = img__alloc(&tmp, src->w, h, src->has_alpha);
        if (rc != IMG_OK)
            return rc;
        for (x = 0; x < src->w; x++)
            resample(src->px + x, sa ? sa + x : 0, src->h, src->w,
                     tmp.px + x, tmp.alpha ? tmp.alpha + x : 0, h, src->w);
        rc = img__alloc(out, w, h, src->has_alpha);
        if (rc != IMG_OK) { img_free(&tmp); return rc; }
        for (y = 0; y < h; y++)
            resample(tmp.px + (long)y * src->w,
                     tmp.alpha ? tmp.alpha + (long)y * src->w : 0, src->w, 1,
                     out->px + (long)y * w,
                     out->alpha ? out->alpha + (long)y * w : 0, w, 1);
    }
    img_free(&tmp);
    return IMG_OK;
}
