/* KestrelOS libimg: QOI (Quite OK Image) and Netpbm (P1-P6) decoders.
 *
 * Pure integer, bounds-checked, zero recursion, minimal stack frames.
 * - QOI: magic "qoif", RGB/RGBA, sRGB/linear, all 6 opcodes + 64-entry
 *   running color index hash + 8-byte 0x00..00 0x01 end marker.
 * - Netpbm: P1/P4 PBM (0=white, 1=black), P2/P5 PGM (8/16-bit gray),
 *   P3/P6 PPM (8/16-bit RGB), including '#' comment skipping.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

static unsigned long rd32be(const uint8_t *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}

/* ================================================================ QOI */

static const uint8_t qoi_end_marker[8] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

struct qoi_px {
    uint8_t r, g, b, a;
};

static int qoi_parse_header(const uint8_t *d, unsigned long len,
                            long *out_w, long *out_h, int *out_ch)
{
    unsigned long w, h;
    int ch, cs;

    if (len < 4)
        return (len == 0 || memcmp(d, "qoif", len) == 0)
            ? IMG_ERR_TRUNCATED : IMG_ERR_FORMAT;
    if (memcmp(d, "qoif", 4) != 0)
        return IMG_ERR_FORMAT;
    if (len < 14)
        return IMG_ERR_TRUNCATED;

    w = rd32be(d + 4);
    h = rd32be(d + 8);
    ch = d[12];
    cs = d[13];

    if (w == 0 || h == 0 || w > 0x7fffffffUL || h > 0x7fffffffUL)
        return IMG_ERR_CORRUPT;
    if ((ch != 3 && ch != 4) || (cs != 0 && cs != 1))
        return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;

    *out_w = (long)w;
    *out_h = (long)h;
    *out_ch = ch;
    return IMG_OK;
}

int qoi_probe(const void *data, unsigned long len, struct img_info *out)
{
    const uint8_t *d = (const uint8_t *)data;
    long w, h;
    int ch, rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->fmt = IMG_FMT_QOI;
    out->frames = 1;
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = qoi_parse_header(d, len, &w, &h, &ch);
    if (rc != IMG_OK)
        return rc;
    out->w = (int)w;
    out->h = (int)h;
    out->has_alpha = (ch == 4) ? 1 : 0;
    return IMG_OK;
}

int qoi_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct qoi_px *index;
    struct qoi_px px;
    struct image im;
    long w, h;
    int ch, rc;
    unsigned long npix, i, pos, chunks_end;
    int run = 0;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = qoi_parse_header(d, len, &w, &h, &ch);
    if (rc != IMG_OK)
        return rc;
    if (len < 14 + 8)
        return IMG_ERR_TRUNCATED;

    npix = (unsigned long)w * (unsigned long)h;
    chunks_end = len - 8;
    if ((uint64_t)(chunks_end - 14) * 62ULL < (uint64_t)npix)
        return IMG_ERR_TRUNCATED;

    index = (struct qoi_px *)calloc(64, sizeof(*index));
    if (!index)
        return IMG_ERR_NOMEM;

    rc = img__alloc(&im, w, h, ch == 4);
    if (rc != IMG_OK) {
        free(index);
        return rc;
    }

    pos = 14;
    px.r = 0;
    px.g = 0;
    px.b = 0;
    px.a = 255;

    for (i = 0; i < npix; i++) {
        if (run > 0) {
            run--;
        } else {
            uint8_t b1;
            unsigned hidx;

            if (pos >= chunks_end) {
                img_free(&im);
                free(index);
                return IMG_ERR_TRUNCATED;
            }
            b1 = d[pos++];
            if (b1 == 0xfe) {
                if (pos + 3 > chunks_end) {
                    img_free(&im);
                    free(index);
                    return IMG_ERR_TRUNCATED;
                }
                px.r = d[pos++];
                px.g = d[pos++];
                px.b = d[pos++];
            } else if (b1 == 0xff) {
                if (pos + 4 > chunks_end) {
                    img_free(&im);
                    free(index);
                    return IMG_ERR_TRUNCATED;
                }
                px.r = d[pos++];
                px.g = d[pos++];
                px.b = d[pos++];
                px.a = d[pos++];
            } else if ((b1 & 0xc0) == 0x00) {
                px = index[b1 & 0x3f];
            } else if ((b1 & 0xc0) == 0x40) {
                int dr = ((b1 >> 4) & 3) - 2;
                int dg = ((b1 >> 2) & 3) - 2;
                int db = (b1 & 3) - 2;
                px.r = (uint8_t)(px.r + dr);
                px.g = (uint8_t)(px.g + dg);
                px.b = (uint8_t)(px.b + db);
            } else if ((b1 & 0xc0) == 0x80) {
                uint8_t b2;
                int dg, dr, db;
                if (pos >= chunks_end) {
                    img_free(&im);
                    free(index);
                    return IMG_ERR_TRUNCATED;
                }
                b2 = d[pos++];
                dg = (b1 & 0x3f) - 32;
                dr = dg + ((b2 >> 4) & 0x0f) - 8;
                db = dg + (b2 & 0x0f) - 8;
                px.r = (uint8_t)(px.r + dr);
                px.g = (uint8_t)(px.g + dg);
                px.b = (uint8_t)(px.b + db);
            } else {
                run = b1 & 0x3f;
                if ((unsigned long)run >= npix - i) {
                    img_free(&im);
                    free(index);
                    return IMG_ERR_CORRUPT;
                }
            }
            hidx = ((unsigned)px.r * 3u + (unsigned)px.g * 5u +
                    (unsigned)px.b * 7u + (unsigned)px.a * 11u) & 63u;
            index[hidx] = px;
        }

        im.px[i] = img__rgb(px.r, px.g, px.b);
        if (im.alpha)
            im.alpha[i] = px.a;
    }

    free(index);
    if (pos + 8 > len) {
        img_free(&im);
        return IMG_ERR_TRUNCATED;
    }
    if (memcmp(d + pos, qoi_end_marker, 8) != 0) {
        img_free(&im);
        return IMG_ERR_CORRUPT;
    }

    *out = im;
    return IMG_OK;
}

/* ================================================================ Netpbm */

static int pnm_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

static void pnm_skip_ws(const uint8_t *d, unsigned long len, unsigned long *pos)
{
    while (*pos < len) {
        uint8_t c = d[*pos];
        if (pnm_ws(c)) {
            (*pos)++;
        } else if (c == '#') {
            (*pos)++;
            while (*pos < len && d[*pos] != '\n' && d[*pos] != '\r')
                (*pos)++;
        } else {
            break;
        }
    }
}

static int pnm_read_uint(const uint8_t *d, unsigned long len,
                         unsigned long *pos, unsigned long *out)
{
    unsigned long v = 0;

    pnm_skip_ws(d, len, pos);
    if (*pos >= len)
        return IMG_ERR_TRUNCATED;
    if (d[*pos] < '0' || d[*pos] > '9')
        return IMG_ERR_CORRUPT;

    while (*pos < len && d[*pos] >= '0' && d[*pos] <= '9') {
        unsigned digit = (unsigned)(d[*pos] - '0');
        if (v > (0x7fffffffUL - digit) / 10UL)
            return IMG_ERR_TOOBIG;
        v = v * 10UL + digit;
        (*pos)++;
    }
    *out = v;
    return IMG_OK;
}

static int pnm_scale(unsigned long v, unsigned long maxval)
{
    if (v > maxval)
        v = maxval;
    if (maxval == 255)
        return (int)v;
    return (int)((v * 255UL + maxval / 2UL) / maxval);
}

static int pnm_parse_header(const uint8_t *d, unsigned long len,
                            int *out_type, long *out_w, long *out_h,
                            unsigned long *out_maxval, unsigned long *out_pos)
{
    unsigned long pos = 0;
    unsigned long w, h, maxval = 1;
    int type, rc;

    if (len < 2)
        return (len == 0 || d[0] == 'P') ? IMG_ERR_TRUNCATED : IMG_ERR_FORMAT;
    if (d[0] != 'P' || d[1] < '1' || d[1] > '6')
        return IMG_ERR_FORMAT;
    type = d[1] - '0';
    if (len < 3)
        return IMG_ERR_TRUNCATED;
    if (!pnm_ws(d[2]) && d[2] != '#')
        return IMG_ERR_FORMAT;

    pos = 2;
    rc = pnm_read_uint(d, len, &pos, &w);
    if (rc != IMG_OK)
        return rc;
    rc = pnm_read_uint(d, len, &pos, &h);
    if (rc != IMG_OK)
        return rc;

    if (w == 0 || h == 0)
        return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;

    if (type != 1 && type != 4) {
        rc = pnm_read_uint(d, len, &pos, &maxval);
        if (rc != IMG_OK)
            return rc;
        if (maxval == 0 || maxval > 65535UL)
            return IMG_ERR_CORRUPT;
    }

    if (type >= 4) {
        unsigned long expect_bytes;

        if (type == 4)
            expect_bytes = ((w + 7UL) / 8UL) * h;
        else {
            unsigned long spp = (type == 6) ? 3UL : 1UL;
            unsigned long bps = (maxval > 255UL) ? 2UL : 1UL;
            expect_bytes = w * h * spp * bps;
        }

        if (pos >= len)
            return IMG_ERR_TRUNCATED;
        if (d[pos] == '#') {
            pos++;
            while (pos < len && d[pos] != '\n' && d[pos] != '\r')
                pos++;
            if (pos >= len)
                return IMG_ERR_TRUNCATED;
        }
        if (!pnm_ws(d[pos]))
            return IMG_ERR_CORRUPT;
        if (d[pos] == '\r' && pos + 1 < len && d[pos + 1] == '\n' &&
            len - (pos + 2) == expect_bytes) {
            pos += 2;
        } else {
            pos += 1;
        }
    }

    *out_type = type;
    *out_w = (long)w;
    *out_h = (long)h;
    *out_maxval = maxval;
    *out_pos = pos;
    return IMG_OK;
}

int pnm_probe(const void *data, unsigned long len, struct img_info *out)
{
    const uint8_t *d = (const uint8_t *)data;
    long w, h;
    unsigned long maxval, pos;
    int type, rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->fmt = IMG_FMT_PNM;
    out->frames = 1;
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = pnm_parse_header(d, len, &type, &w, &h, &maxval, &pos);
    if (rc != IMG_OK)
        return rc;
    out->w = (int)w;
    out->h = (int)h;
    out->has_alpha = 0;
    return IMG_OK;
}

static int pnm_decode_p1(const uint8_t *d, unsigned long len,
                         unsigned long pos, struct image *im)
{
    unsigned long npix = (unsigned long)im->w * (unsigned long)im->h;
    unsigned long i;

    for (i = 0; i < npix; i++) {
        pnm_skip_ws(d, len, &pos);
        if (pos >= len)
            return IMG_ERR_TRUNCATED;
        if (d[pos] == '0')
            im->px[i] = img__rgb(255, 255, 255);
        else if (d[pos] == '1')
            im->px[i] = img__rgb(0, 0, 0);
        else
            return IMG_ERR_CORRUPT;
        pos++;
    }
    return IMG_OK;
}

static int pnm_decode_ascii(const uint8_t *d, unsigned long len,
                            unsigned long pos, int type,
                            unsigned long maxval, struct image *im)
{
    unsigned long npix = (unsigned long)im->w * (unsigned long)im->h;
    unsigned long i;

    for (i = 0; i < npix; i++) {
        if (type == 2) {
            unsigned long v;
            int g, rc = pnm_read_uint(d, len, &pos, &v);
            if (rc != IMG_OK)
                return rc;
            g = pnm_scale(v, maxval);
            im->px[i] = img__rgb(g, g, g);
        } else {
            unsigned long r, g, b;
            int rc;
            if ((rc = pnm_read_uint(d, len, &pos, &r)) != IMG_OK ||
                (rc = pnm_read_uint(d, len, &pos, &g)) != IMG_OK ||
                (rc = pnm_read_uint(d, len, &pos, &b)) != IMG_OK)
                return rc;
            im->px[i] = img__rgb(pnm_scale(r, maxval),
                                 pnm_scale(g, maxval),
                                 pnm_scale(b, maxval));
        }
    }
    return IMG_OK;
}

static int pnm_decode_bin(const uint8_t *d, unsigned long len,
                          unsigned long pos, int type,
                          unsigned long maxval, struct image *im)
{
    long y, x;
    long w = im->w, h = im->h;

    if (type == 4) {
        unsigned long row_bytes = ((unsigned long)w + 7UL) / 8UL;
        unsigned long need = row_bytes * (unsigned long)h;
        if (need > len - pos)
            return IMG_ERR_TRUNCATED;
        for (y = 0; y < h; y++) {
            const uint8_t *row = d + pos + (unsigned long)y * row_bytes;
            uint32_t *dst = im->px + (long)y * w;
            for (x = 0; x < w; x++) {
                int bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
                dst[x] = bit ? img__rgb(0, 0, 0) : img__rgb(255, 255, 255);
            }
        }
        return IMG_OK;
    }

    {
        unsigned long spp = (type == 6) ? 3UL : 1UL;
        unsigned long bps = (maxval > 255UL) ? 2UL : 1UL;
        unsigned long npix = (unsigned long)w * (unsigned long)h;
        unsigned long need = npix * spp * bps;
        unsigned long i;

        if (need > len - pos)
            return IMG_ERR_TRUNCATED;

        for (i = 0; i < npix; i++) {
            if (type == 5) {
                unsigned long v = (bps == 2)
                    ? (((unsigned long)d[pos] << 8) | d[pos + 1])
                    : (unsigned long)d[pos];
                int g = pnm_scale(v, maxval);
                pos += bps;
                im->px[i] = img__rgb(g, g, g);
            } else {
                unsigned long r, g, b;
                if (bps == 2) {
                    r = ((unsigned long)d[pos] << 8) | d[pos + 1];
                    g = ((unsigned long)d[pos + 2] << 8) | d[pos + 3];
                    b = ((unsigned long)d[pos + 4] << 8) | d[pos + 5];
                } else {
                    r = d[pos];
                    g = d[pos + 1];
                    b = d[pos + 2];
                }
                pos += 3UL * bps;
                im->px[i] = img__rgb(pnm_scale(r, maxval),
                                     pnm_scale(g, maxval),
                                     pnm_scale(b, maxval));
            }
        }
    }
    return IMG_OK;
}

int pnm_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct image im;
    long w, h;
    unsigned long maxval, pos;
    int type, rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = pnm_parse_header(d, len, &type, &w, &h, &maxval, &pos);
    if (rc != IMG_OK)
        return rc;

    if (type >= 4) {
        unsigned long need;
        if (type == 4)
            need = (((unsigned long)w + 7UL) / 8UL) * (unsigned long)h;
        else {
            unsigned long spp = (type == 6) ? 3UL : 1UL;
            unsigned long bps = (maxval > 255UL) ? 2UL : 1UL;
            need = (unsigned long)w * (unsigned long)h * spp * bps;
        }
        if (need > len - pos)
            return IMG_ERR_TRUNCATED;
    } else {
        unsigned long min_ascii = (unsigned long)w * (unsigned long)h *
                                  (type == 3 ? 3UL : 1UL);
        if (min_ascii > len - pos)
            return IMG_ERR_TRUNCATED;
    }

    rc = img__alloc(&im, w, h, 0);
    if (rc != IMG_OK)
        return rc;

    if (type == 1)
        rc = pnm_decode_p1(d, len, pos, &im);
    else if (type == 2 || type == 3)
        rc = pnm_decode_ascii(d, len, pos, type, maxval, &im);
    else
        rc = pnm_decode_bin(d, len, pos, type, maxval, &im);

    if (rc != IMG_OK) {
        img_free(&im);
        return rc;
    }

    *out = im;
    return IMG_OK;
}
