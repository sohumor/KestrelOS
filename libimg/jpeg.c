/* KestrelOS libimg: baseline sequential DCT JPEG (ITU-T T.81).
 *
 * Supported: SOI, APPn, COM, DQT (8- and 16-bit tables), SOF0/SOF1, DHT,
 * SOS, DRI with RSTn restart markers, EOI; Huffman entropy coding; 1- and
 * 3-component images with any sampling factors in 1..4, which covers
 * 4:4:4, 4:2:2, 4:2:0, 4:1:1 and 4:4:0; an integer inverse DCT; YCbCr to
 * RGB.
 *
 * Not supported, and reported as such rather than decoded into garbage:
 * progressive (SOF2), arithmetic coding (SOF9/SOF10), lossless and
 * hierarchical modes, 12-bit precision, and 4-component (CMYK/YCCK) data.
 *
 * The IDCT is derived here rather than lifted: the 8-point transform is
 * split into its even and odd halves, the even half collapsing to four
 * butterflies and the odd half to a 4x4 matrix of cosines, all in 13-bit
 * fixed point over 64-bit accumulators so no input can overflow it.
 */

#include <stdlib.h>
#include <string.h>

#include "img.h"

/* Refuse component planes larger than this many samples each; with the
 * 8192-pixel axis cap a plane can round up to one MCU past the image. */
#define JPEG_MAX_PLANE  (80UL * 1024UL * 1024UL)

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ------------------------------------------------------------ tables */

struct huff {
    int      present;
    int      nvals;
    uint8_t  vals[256];
    int      mincode[17];
    int      maxcode[17];   /* -1 when no code has this length */
    int      valptr[17];
    int32_t  lut[256];      /* (len << 8) | value for codes <= 8 bits, else -1 */
};

struct comp {
    int id, h, v, tq;
    int td, ta;
    long dcpred;
    long pw, ph;            /* plane size, a whole number of MCUs */
    uint8_t *plane;
};

struct jpg {
    const uint8_t *d;
    unsigned long len, pos;

    uint16_t qt[4][64];      /* still in zigzag order, as stored */
    int      qt_present[4];
    struct huff hdc[4], hac[4];

    struct comp comp[4];
    int ncomp;
    long w, h;
    int hmax, vmax;
    long mcux, mcuy;
    long restart_interval;

    /* entropy bit reader */
    uint32_t bb;
    int      bc;
    int      hit_marker;
};

/* ------------------------------------------------------------ bit reader
 *
 * The entropy stream stuffs a zero byte after every 0xff. A 0xff followed
 * by anything else is the next marker: we stop there, back up so the
 * caller can see it, and feed zero bits so decoding unwinds instead of
 * reading past the end.
 */

static void bit_fill(struct jpg *j)
{
    while (j->bc <= 24) {
        int c = 0;

        if (!j->hit_marker) {
            if (j->pos >= j->len) {
                j->hit_marker = 1;
            } else {
                c = j->d[j->pos++];
                if (c == 0xff) {
                    int c2 = (j->pos < j->len) ? j->d[j->pos] : 0xd9;
                    if (c2 == 0) {
                        j->pos++;            /* stuffed: a literal 0xff */
                    } else {
                        j->pos--;            /* leave the marker in place */
                        j->hit_marker = 1;
                        c = 0;
                    }
                }
            }
        }
        j->bb = (j->bb << 8) | (uint32_t)(unsigned)c;
        j->bc += 8;
    }
}

static unsigned bit_peek(struct jpg *j, int n)
{
    bit_fill(j);
    return (j->bb >> (j->bc - n)) & ((1u << n) - 1);
}

static unsigned bit_get(struct jpg *j, int n)
{
    unsigned v;

    if (n <= 0)
        return 0;
    bit_fill(j);
    v = (j->bb >> (j->bc - n)) & ((1u << n) - 1);
    j->bc -= n;
    return v;
}

/* The magnitude-category decoding of T.81 F.2.2.1. */
static long extend(unsigned v, int s)
{
    if (s == 0)
        return 0;
    if (v < (1u << (s - 1)))
        return (long)v - (long)(1u << s) + 1;
    return (long)v;
}

/* ------------------------------------------------------------ huffman */

static int huff_build(struct huff *t, const uint8_t *counts,
                      const uint8_t *vals, int nvals)
{
    int l, i, code = 0, k = 0;

    memset(t, 0, sizeof(*t));
    t->nvals = nvals;
    memcpy(t->vals, vals, (unsigned)nvals);
    for (i = 0; i < 256; i++)
        t->lut[i] = -1;

    for (l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        if (counts[l - 1] == 0) {
            t->maxcode[l] = -1;
        } else {
            /* Fill the 8-bit fast path while the codes are still short. */
            if (l <= 8) {
                for (i = 0; i < counts[l - 1]; i++) {
                    int c = code + i;
                    int base = c << (8 - l);
                    int n = 1 << (8 - l);
                    int q;
                    if (k + i >= nvals)
                        return IMG_ERR_CORRUPT;
                    for (q = 0; q < n; q++) {
                        if (base + q > 255)
                            return IMG_ERR_CORRUPT;
                        t->lut[base + q] = (l << 8) | t->vals[k + i];
                    }
                }
            }
            code += counts[l - 1];
            k += counts[l - 1];
            t->maxcode[l] = code - 1;
        }
        /* A canonical table must never need more codes of a length than
         * that length can express. */
        if (code > (1 << l) || k > nvals)
            return IMG_ERR_CORRUPT;
        code <<= 1;
    }
    t->present = 1;
    return IMG_OK;
}

static int huff_decode(struct jpg *j, const struct huff *t)
{
    int code, l, idx;
    int32_t e;

    if (!t->present)
        return -1;

    e = t->lut[bit_peek(j, 8)];
    if (e >= 0) {
        j->bc -= (e >> 8);
        return e & 0xff;
    }

    /* Longer than 8 bits: walk the canonical table one bit at a time. */
    code = (int)bit_get(j, 8);
    for (l = 9; l <= 16; l++) {
        code = (code << 1) | (int)bit_get(j, 1);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l]) {
            idx = t->valptr[l] + code - t->mincode[l];
            if (idx < 0 || idx >= t->nvals)
                return -1;
            return t->vals[idx];
        }
    }
    return -1;
}

/* ------------------------------------------------------------ IDCT
 *
 * cos(n*pi/16) in 13-bit fixed point.
 */
#define C1 8035
#define C2 7568
#define C3 6811
#define C4 5793
#define C5 4551
#define C6 3135
#define C7 1598

/* One 8-point inverse transform, scaled by 8192 and descaled by `shift`.
 * Even half: the four contributions collapse to two butterflies plus one
 * rotation. Odd half: a direct 4x4 cosine matrix, symmetric about the
 * midpoint with a sign flip, which is what gives the mirrored outputs. */
static void idct_1d(const long *f, int is, long *out, int os, int shift)
{
    long f0 = f[0], f1 = f[is], f2 = f[2 * is], f3 = f[3 * is];
    long f4 = f[4 * is], f5 = f[5 * is], f6 = f[6 * is], f7 = f[7 * is];
    long a, b, c, d, g0, g1, g2, g3, h0, h1, h2, h3;
    long r = 1L << (shift - 1);

    a = C4 * (f0 + f4);
    b = C4 * (f0 - f4);
    c = C2 * f2 + C6 * f6;
    d = C6 * f2 - C2 * f6;
    g0 = a + c;
    g1 = b + d;
    g2 = b - d;
    g3 = a - c;

    h0 =  C1 * f1 + C3 * f3 + C5 * f5 + C7 * f7;
    h1 =  C3 * f1 - C7 * f3 - C1 * f5 - C5 * f7;
    h2 =  C5 * f1 - C1 * f3 + C7 * f5 + C3 * f7;
    h3 =  C7 * f1 - C5 * f3 + C3 * f5 - C1 * f7;

    out[0]      = (g0 + h0 + r) >> shift;
    out[os]     = (g1 + h1 + r) >> shift;
    out[2 * os] = (g2 + h2 + r) >> shift;
    out[3 * os] = (g3 + h3 + r) >> shift;
    out[4 * os] = (g3 - h3 + r) >> shift;
    out[5 * os] = (g2 - h2 + r) >> shift;
    out[6 * os] = (g1 - h1 + r) >> shift;
    out[7 * os] = (g0 - h0 + r) >> shift;
}

/* Rows keep two extra fractional bits (>>11 of the 13-bit scale); the
 * column pass then removes those two, the second 13-bit scale, and the
 * two halvings the transform definition carries: 2+13+2 = 17. */
static void idct_block(const long *coef, uint8_t *dst, long stride)
{
    long tmp[64], col[8];
    int i, k;

    for (i = 0; i < 8; i++)
        idct_1d(coef + i * 8, 1, tmp + i * 8, 1, 11);
    for (i = 0; i < 8; i++) {
        idct_1d(tmp + i, 8, col, 1, 17);
        for (k = 0; k < 8; k++)
            dst[(long)k * stride + i] = (uint8_t)img__clamp8(col[k] + 128);
    }
}

/* ------------------------------------------------------------ scan */

static int decode_block(struct jpg *j, struct comp *ci, uint8_t *dst,
                        long stride)
{
    long blk[64];
    const uint16_t *q = j->qt[ci->tq];
    int t, k;

    memset(blk, 0, sizeof(blk));

    t = huff_decode(j, &j->hdc[ci->td]);
    if (t < 0 || t > 15)
        return IMG_ERR_CORRUPT;
    ci->dcpred += extend(bit_get(j, t), t);
    blk[0] = ci->dcpred * (long)q[0];

    k = 1;
    while (k < 64) {
        int rs = huff_decode(j, &j->hac[ci->ta]);
        int s, r;

        if (rs < 0)
            return IMG_ERR_CORRUPT;
        s = rs & 15;
        r = rs >> 4;
        if (s == 0) {
            if (r != 15)
                break;                 /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            break;
        blk[zigzag[k]] = extend(bit_get(j, s), s) * (long)q[k];
        k++;
    }

    idct_block(blk, dst, stride);
    return IMG_OK;
}

/* Resynchronise at a restart marker: byte-align, find the RSTn, and clear
 * the DC predictors. */
static int do_restart(struct jpg *j)
{
    int i;

    j->bb = 0;
    j->bc = 0;
    j->hit_marker = 0;

    while (j->pos + 1 < j->len) {
        if (j->d[j->pos] == 0xff && j->d[j->pos + 1] != 0)
            break;
        j->pos++;
    }
    if (j->pos + 1 >= j->len)
        return IMG_ERR_TRUNCATED;
    if (j->d[j->pos + 1] < 0xd0 || j->d[j->pos + 1] > 0xd7)
        return IMG_ERR_CORRUPT;
    j->pos += 2;
    for (i = 0; i < j->ncomp; i++)
        j->comp[i].dcpred = 0;
    return IMG_OK;
}

static int decode_scan(struct jpg *j)
{
    long mx, my, seen = 0;
    int i, rc;

    for (i = 0; i < j->ncomp; i++)
        j->comp[i].dcpred = 0;
    j->bb = 0;
    j->bc = 0;
    j->hit_marker = 0;

    for (my = 0; my < j->mcuy; my++) {
        for (mx = 0; mx < j->mcux; mx++) {
            if (j->restart_interval && seen && seen % j->restart_interval == 0) {
                rc = do_restart(j);
                if (rc != IMG_OK)
                    return rc;
            }
            for (i = 0; i < j->ncomp; i++) {
                struct comp *ci = &j->comp[i];
                int bx, by;

                for (by = 0; by < ci->v; by++) {
                    for (bx = 0; bx < ci->h; bx++) {
                        long px = (mx * ci->h + bx) * 8;
                        long py = (my * ci->v + by) * 8;

                        rc = decode_block(j, ci,
                                          ci->plane + py * ci->pw + px,
                                          ci->pw);
                        if (rc != IMG_OK)
                            return rc;
                    }
                }
            }
            seen++;
        }
    }
    return IMG_OK;
}

/* ------------------------------------------------------------ colour */

/* 16-bit fixed point BT.601 inverse, as in JFIF. */
#define YR  91881L   /* 1.402   */
#define YGB 22554L   /* 0.34414 */
#define YGR 46802L   /* 0.71414 */
#define YB 116130L   /* 1.772   */

static void to_rgb(struct jpg *j, struct image *im)
{
    long x, y;

    for (y = 0; y < j->h; y++) {
        uint32_t *dst = im->px + y * j->w;

        if (j->ncomp == 1) {
            const struct comp *c0 = &j->comp[0];
            const uint8_t *sy = c0->plane + y * c0->pw;
            for (x = 0; x < j->w; x++) {
                int v = sy[x];
                dst[x] = img__rgb(v, v, v);
            }
            continue;
        }
        for (x = 0; x < j->w; x++) {
            const struct comp *c0 = &j->comp[0];
            const struct comp *c1 = &j->comp[1];
            const struct comp *c2 = &j->comp[2];
            long y0 = (y * c0->v) / j->vmax, x0 = (x * c0->h) / j->hmax;
            long y1 = (y * c1->v) / j->vmax, x1 = (x * c1->h) / j->hmax;
            long y2 = (y * c2->v) / j->vmax, x2 = (x * c2->h) / j->hmax;
            long Y, cb, cr;

            if (x0 >= c0->pw) x0 = c0->pw - 1;
            if (y0 >= c0->ph) y0 = c0->ph - 1;
            if (x1 >= c1->pw) x1 = c1->pw - 1;
            if (y1 >= c1->ph) y1 = c1->ph - 1;
            if (x2 >= c2->pw) x2 = c2->pw - 1;
            if (y2 >= c2->ph) y2 = c2->ph - 1;

            Y  = c0->plane[y0 * c0->pw + x0];
            cb = (long)c1->plane[y1 * c1->pw + x1] - 128;
            cr = (long)c2->plane[y2 * c2->pw + x2] - 128;

            dst[x] = img__rgb(img__clamp8(Y + ((YR * cr + 32768) >> 16)),
                              img__clamp8(Y - ((YGB * cb + YGR * cr + 32768) >> 16)),
                              img__clamp8(Y + ((YB * cb + 32768) >> 16)));
        }
    }
}

/* ------------------------------------------------------------ markers */

static unsigned rd16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

static void jpg_cleanup(struct jpg *j)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (j->comp[i].plane) {
            free(j->comp[i].plane);
            j->comp[i].plane = 0;
        }
    }
}

static int parse_sof(struct jpg *j, const uint8_t *p, unsigned long n)
{
    unsigned long need;
    int i;

    if (n < 6)
        return IMG_ERR_CORRUPT;
    if (p[0] != 8)
        return IMG_ERR_UNSUPPORTED;            /* only 8-bit precision */
    j->h = (long)rd16(p + 1);
    j->w = (long)rd16(p + 3);
    j->ncomp = p[5];
    if (j->w <= 0 || j->h <= 0)
        return IMG_ERR_CORRUPT;
    if (j->w > IMG_MAX_DIM || j->h > IMG_MAX_DIM ||
        (unsigned long)j->w * (unsigned long)j->h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;
    if (j->ncomp != 1 && j->ncomp != 3)
        return IMG_ERR_UNSUPPORTED;            /* CMYK/YCCK needs APP14 */
    if (n < 6 + (unsigned long)j->ncomp * 3)
        return IMG_ERR_CORRUPT;

    j->hmax = j->vmax = 1;
    for (i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
            return IMG_ERR_CORRUPT;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    /* A single-component image is coded as one block per MCU whatever the
     * sampling factors claim (T.81 A.2.2), so normalise them away. */
    if (j->ncomp == 1) {
        j->comp[0].h = j->comp[0].v = 1;
        j->hmax = j->vmax = 1;
    }

    j->mcux = (j->w + (long)j->hmax * 8 - 1) / ((long)j->hmax * 8);
    j->mcuy = (j->h + (long)j->vmax * 8 - 1) / ((long)j->vmax * 8);

    for (i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->pw = j->mcux * c->h * 8;
        c->ph = j->mcuy * c->v * 8;
        need = (unsigned long)c->pw * (unsigned long)c->ph;
        if (need == 0 || need > JPEG_MAX_PLANE)
            return IMG_ERR_TOOBIG;
        if (c->plane)
            return IMG_ERR_CORRUPT;            /* a second SOF */
        c->plane = (uint8_t *)malloc(need);
        if (!c->plane)
            return IMG_ERR_NOMEM;
        memset(c->plane, 128, need);
    }
    return IMG_OK;
}

static int parse_sos(struct jpg *j, const uint8_t *p, unsigned long n)
{
    int ns, i, k;

    if (n < 1)
        return IMG_ERR_CORRUPT;
    ns = p[0];
    if (n < 1 + (unsigned long)ns * 2 + 3)
        return IMG_ERR_CORRUPT;
    if (ns != j->ncomp)
        return IMG_ERR_UNSUPPORTED;            /* non-interleaved multi-scan */

    for (i = 0; i < ns; i++) {
        int cs = p[1 + i * 2];
        int tt = p[2 + i * 2];
        for (k = 0; k < j->ncomp; k++) {
            if (j->comp[k].id == cs) {
                j->comp[k].td = tt >> 4;
                j->comp[k].ta = tt & 15;
                if (j->comp[k].td > 3 || j->comp[k].ta > 3)
                    return IMG_ERR_CORRUPT;
                break;
            }
        }
        if (k == j->ncomp)
            return IMG_ERR_CORRUPT;
    }
    return IMG_OK;
}

/* ------------------------------------------------------------ decode */

int jpeg_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct jpg *j;
    struct image im;
    int rc = IMG_OK, seen_sof = 0, done = 0, i;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;
    if (len < 4)
        return IMG_ERR_TRUNCATED;
    if (d[0] != 0xff || d[1] != 0xd8)
        return IMG_ERR_FORMAT;

    j = (struct jpg *)malloc(sizeof(struct jpg));
    if (!j)
        return IMG_ERR_NOMEM;
    memset(j, 0, sizeof(*j));
    j->d = d;
    j->len = len;
    j->pos = 2;

    while (!done) {
        unsigned m, seglen;
        const uint8_t *body;

        /* Markers may be preceded by any number of fill bytes. */
        while (j->pos < len && d[j->pos] != 0xff)
            j->pos++;
        while (j->pos < len && d[j->pos] == 0xff)
            j->pos++;
        if (j->pos >= len) {
            rc = seen_sof ? IMG_ERR_TRUNCATED : IMG_ERR_CORRUPT;
            goto out;
        }
        m = d[j->pos++];

        if (m == 0xd9)                          /* EOI */
            break;
        if (m == 0x01 || m == 0xd8 || (m >= 0xd0 && m <= 0xd7))
            continue;                           /* standalone markers */

        if (j->pos + 2 > len) {
            rc = IMG_ERR_TRUNCATED;
            goto out;
        }
        seglen = rd16(d + j->pos);
        if (seglen < 2 || j->pos + seglen > len) {
            rc = IMG_ERR_TRUNCATED;
            goto out;
        }
        body = d + j->pos + 2;
        seglen -= 2;

        switch (m) {
        case 0xc0:                              /* SOF0 baseline */
        case 0xc1:                              /* SOF1 extended sequential */
            if (seen_sof) {
                rc = IMG_ERR_CORRUPT;
                goto out;
            }
            rc = parse_sof(j, body, seglen);
            if (rc != IMG_OK)
                goto out;
            seen_sof = 1;
            break;

        case 0xc2:                              /* SOF2 */
            rc = IMG_ERR_PROGRESSIVE;
            goto out;

        case 0xc3: case 0xc5: case 0xc6: case 0xc7:
        case 0xc9: case 0xca: case 0xcb:
        case 0xcd: case 0xce: case 0xcf:
            rc = IMG_ERR_UNSUPPORTED;           /* lossless / arithmetic */
            goto out;

        case 0xc4: {                            /* DHT */
            unsigned long o = 0;
            while (o + 17 <= seglen) {
                int tc = body[o] >> 4, th = body[o] & 15;
                int total = 0, l;
                for (l = 0; l < 16; l++)
                    total += body[o + 1 + l];
                if (tc > 1 || th > 3 || total > 256 ||
                    o + 17 + (unsigned long)total > seglen) {
                    rc = IMG_ERR_CORRUPT;
                    goto out;
                }
                rc = huff_build(tc ? &j->hac[th] : &j->hdc[th],
                                body + o + 1, body + o + 17, total);
                if (rc != IMG_OK)
                    goto out;
                o += 17 + (unsigned long)total;
            }
            break;
        }

        case 0xdb: {                            /* DQT */
            unsigned long o = 0;
            while (o < seglen) {
                int pq = body[o] >> 4, tq = body[o] & 15;
                int k;
                if (tq > 3 || pq > 1) {
                    rc = IMG_ERR_CORRUPT;
                    goto out;
                }
                o++;
                if (o + (pq ? 128UL : 64UL) > seglen) {
                    rc = IMG_ERR_CORRUPT;
                    goto out;
                }
                for (k = 0; k < 64; k++)
                    j->qt[tq][k] = pq ? (uint16_t)rd16(body + o + k * 2)
                                      : body[o + k];
                j->qt_present[tq] = 1;
                o += pq ? 128UL : 64UL;
            }
            break;
        }

        case 0xdd:                              /* DRI */
            if (seglen < 2) {
                rc = IMG_ERR_CORRUPT;
                goto out;
            }
            j->restart_interval = (long)rd16(body);
            break;

        case 0xda: {                            /* SOS */
            if (!seen_sof) {
                rc = IMG_ERR_CORRUPT;
                goto out;
            }
            rc = parse_sos(j, body, seglen);
            if (rc != IMG_OK)
                goto out;
            for (i = 0; i < j->ncomp; i++) {
                if (!j->qt_present[j->comp[i].tq]) {
                    rc = IMG_ERR_CORRUPT;
                    goto out;
                }
            }
            j->pos += seglen + 2;
            rc = decode_scan(j);
            if (rc != IMG_OK)
                goto out;
            done = 1;
            continue;                           /* pos already advanced */
        }

        default:
            break;                              /* APPn, COM, DNL, ... */
        }
        j->pos += seglen + 2;
    }

    if (!seen_sof || !done) {
        rc = seen_sof ? IMG_ERR_TRUNCATED : IMG_ERR_CORRUPT;
        goto out;
    }

    rc = img__alloc(&im, j->w, j->h, 0);
    if (rc != IMG_OK)
        goto out;
    to_rgb(j, &im);
    *out = im;
    rc = IMG_OK;

out:
    jpg_cleanup(j);
    free(j);
    if (rc != IMG_OK)
        img_free(out);
    return rc;
}
