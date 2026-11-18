/* KestrelOS libimg: Windows ICO / CUR decoder.
 *
 * Supports multi-entry .ico (type 1) and .cur (type 2) directories,
 * selecting the highest-resolution entry (<= 256x256, where directory 0
 * means 256) and breaking ties by highest bit depth.
 *
 * Selected entries starting with the 8-byte PNG signature are delegated
 * directly to png_decode(). Headerless DIB entries (BITMAPINFOHEADER,
 * biSize >= 40) support 1-, 4-, 8-bit paletted and 24-/32-bit truecolor
 * XOR bitmaps combined with 1-bit AND transparency masks, in both
 * bottom-up and top-down row orders.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

static const uint8_t ico_png_sig[8] = {
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
};

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const uint8_t *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

struct ico_entry {
    const uint8_t *data;
    unsigned long len;
    int dir_w;
    int dir_h;
    int bpp;
    int is_png;
};

static int ico_entry_bpp(const uint8_t *dir_ent, int dir_type,
                         const uint8_t *payload, unsigned long sz,
                         int *out_is_png)
{
    int bpp = 0;

    *out_is_png = 0;
    if (sz >= 8 && memcmp(payload, ico_png_sig, 8) == 0) {
        *out_is_png = 1;
        if (sz >= 26 && memcmp(payload + 12, "IHDR", 4) == 0) {
            int bd = payload[24];
            int ct = payload[25];
            if (ct == 6)      bpp = 4 * bd;
            else if (ct == 2) bpp = 3 * bd;
            else if (ct == 4) bpp = 2 * bd;
            else              bpp = bd;
        }
        return bpp > 0 ? bpp : 32;
    }
    if (sz >= 40) {
        unsigned long bi_size = rd32(payload);
        if (bi_size >= 40)
            bpp = (int)rd16(payload + 14);
    }
    if (bpp <= 0 && dir_type == 1)
        bpp = (int)rd16(dir_ent + 6);
    return bpp > 0 ? bpp : 1;
}

static int ico_select(const uint8_t *d, unsigned long len,
                      struct ico_entry *out)
{
    unsigned reserved, type, count, i;
    unsigned long dir_end;
    unsigned long best_area = 0;
    int best_bpp = -1;
    int have = 0;

    if (len < 6)
        return IMG_ERR_TRUNCATED;
    reserved = rd16(d);
    type = rd16(d + 2);
    count = rd16(d + 4);
    if (reserved != 0 || (type != 1 && type != 2))
        return IMG_ERR_FORMAT;
    if (count == 0)
        return IMG_ERR_CORRUPT;

    dir_end = 6UL + (unsigned long)count * 16UL;
    if (dir_end > len)
        return IMG_ERR_TRUNCATED;

    for (i = 0; i < count; i++) {
        const uint8_t *e = d + 6 + (unsigned long)i * 16UL;
        int ew = e[0] == 0 ? 256 : (int)e[0];
        int eh = e[1] == 0 ? 256 : (int)e[1];
        unsigned long sz = rd32(e + 8);
        unsigned long off = rd32(e + 12);
        unsigned long area;
        int bpp, is_png;

        if (sz == 0)
            return IMG_ERR_CORRUPT;
        if (off >= len || sz > len - off)
            return IMG_ERR_TRUNCATED;

        bpp = ico_entry_bpp(e, (int)type, d + off, sz, &is_png);
        area = (unsigned long)ew * (unsigned long)eh;
        if (!have || area > best_area ||
            (area == best_area && bpp > best_bpp)) {
            have = 1;
            best_area = area;
            best_bpp = bpp;
            out->data = d + off;
            out->len = sz;
            out->dir_w = ew;
            out->dir_h = eh;
            out->bpp = bpp;
            out->is_png = is_png;
        }
    }

    return have ? IMG_OK : IMG_ERR_CORRUPT;
}

static int ico_dib_dims(const uint8_t *p, unsigned long sz, int dir_h,
                        long *out_w, long *out_h, int *out_top_down,
                        int *out_has_and, unsigned long *out_bi_size)
{
    unsigned long bi_size;
    int64_t w, bih, abs_h;

    if (sz < 40)
        return IMG_ERR_TRUNCATED;
    bi_size = rd32(p);
    if (bi_size < 40)
        return (bi_size == 12) ? IMG_ERR_UNSUPPORTED : IMG_ERR_CORRUPT;
    if (bi_size > sz)
        return IMG_ERR_TRUNCATED;

    w = (int64_t)(int32_t)(uint32_t)rd32(p + 4);
    bih = (int64_t)(int32_t)(uint32_t)rd32(p + 8);
    if (w <= 0 || bih == 0)
        return IMG_ERR_CORRUPT;

    *out_top_down = (bih < 0) ? 1 : 0;
    abs_h = bih < 0 ? -bih : bih;

    if (abs_h == (int64_t)dir_h * 2) {
        *out_h = dir_h;
        *out_has_and = 1;
    } else if (abs_h == (int64_t)dir_h) {
        *out_h = dir_h;
        *out_has_and = 0;
    } else if ((abs_h & 1) == 0 && abs_h / 2 > 0) {
        if (abs_h / 2 > IMG_MAX_DIM)
            return IMG_ERR_TOOBIG;
        *out_h = (long)(abs_h / 2);
        *out_has_and = 1;
    } else {
        return IMG_ERR_CORRUPT;
    }

    if (w > IMG_MAX_DIM || *out_h > IMG_MAX_DIM ||
        (unsigned long)w * (unsigned long)(*out_h) > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;

    *out_w = (long)w;
    *out_bi_size = bi_size;
    return IMG_OK;
}

static void ico_mask_info(unsigned long m, int *shift, int *bits)
{
    int s = 0, n = 0;

    if (m == 0) {
        *shift = 0;
        *bits = 0;
        return;
    }
    while (!(m & 1)) {
        m >>= 1;
        s++;
    }
    while (m & 1) {
        m >>= 1;
        n++;
    }
    *shift = s;
    *bits = n;
}

static int ico_mask_get(unsigned long v, int shift, int bits)
{
    unsigned long x;

    if (bits <= 0)
        return 0;
    x = (v >> shift) & ((1UL << bits) - 1);
    if (bits >= 8)
        return (int)(x >> (bits - 8));
    return (int)((x * 255UL) / ((1UL << bits) - 1));
}

int ico_probe(const void *data, unsigned long len, struct img_info *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct ico_entry ent;
    long w, h;
    int top_down, has_and, bpp, rc;
    unsigned long bi_size;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->fmt = IMG_FMT_ICO;
    out->frames = 1;
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = ico_select(d, len, &ent);
    if (rc != IMG_OK)
        return rc;

    if (ent.is_png) {
        struct img_info sub;
        rc = img_probe(ent.data, ent.len, &sub);
        if (rc != IMG_OK)
            return rc;
        out->fmt = IMG_FMT_ICO;
        out->w = sub.w;
        out->h = sub.h;
        out->frames = 1;
        out->has_alpha = sub.has_alpha;
        return IMG_OK;
    }

    rc = ico_dib_dims(ent.data, ent.len, ent.dir_h, &w, &h,
                      &top_down, &has_and, &bi_size);
    if (rc != IMG_OK)
        return rc;
    (void)top_down;
    (void)bi_size;

    bpp = (int)rd16(ent.data + 14);
    out->w = (int)w;
    out->h = (int)h;
    out->has_alpha = (has_and || bpp == 32) ? 1 : 0;
    return IMG_OK;
}

enum {
    ICO_ALPHA_OPAQUE = 0,
    ICO_ALPHA_AND    = 1,
    ICO_ALPHA_BGRA   = 2
};

struct ico_dib_ctx {
    const uint8_t *p;
    unsigned long sz;
    long w, h;
    int bpp, top_down, has_and;
    unsigned long comp;
    unsigned long pix_off, and_off;
    unsigned long xor_stride, and_stride;
    int sr, br, sg, bg, sb, bb, sa, ba;
    int alpha_mode;
    uint32_t *pal;
};

static int ico_check_bounds(struct ico_dib_ctx *c)
{
    unsigned long xor_rowbytes = ((unsigned long)c->w * (unsigned)c->bpp + 7UL) / 8UL;
    unsigned long and_rowbytes = ((unsigned long)c->w + 7UL) / 8UL;
    unsigned long xor_total, need;

    if (c->pix_off >= c->sz)
        return IMG_ERR_TRUNCATED;

    c->xor_stride = (((unsigned long)c->w * (unsigned)c->bpp + 31UL) / 32UL) * 4UL;
    c->and_stride = (((unsigned long)c->w + 31UL) / 32UL) * 4UL;

    if (c->has_and) {
        xor_total = (unsigned long)c->h * c->xor_stride;
        if (xor_total > c->sz - c->pix_off)
            return IMG_ERR_TRUNCATED;
        c->and_off = c->pix_off + xor_total;
        need = (unsigned long)(c->h - 1) * c->and_stride + and_rowbytes;
        if (need > c->sz - c->and_off)
            return IMG_ERR_TRUNCATED;
    } else {
        need = (unsigned long)(c->h - 1) * c->xor_stride + xor_rowbytes;
        if (need > c->sz - c->pix_off)
            return IMG_ERR_TRUNCATED;
    }
    return IMG_OK;
}

static int ico_scan_alpha(struct ico_dib_ctx *c)
{
    long y, x;
    int any_and = 0;
    int seen_a0 = 0, seen_a255 = 0, seen_amid = 0;

    if (c->has_and) {
        for (y = 0; y < c->h && !any_and; y++) {
            const uint8_t *arow = c->p + c->and_off + (unsigned long)y * c->and_stride;
            for (x = 0; x < c->w; x++) {
                if ((arow[x >> 3] >> (7 - (x & 7))) & 1) {
                    any_and = 1;
                    break;
                }
            }
        }
    }

    if (c->bpp != 32 || (c->comp == 3 && c->ba == 0)) {
        c->alpha_mode = any_and ? ICO_ALPHA_AND : ICO_ALPHA_OPAQUE;
        return any_and ? 1 : 0;
    }

    for (y = 0; y < c->h; y++) {
        const uint8_t *xrow = c->p + c->pix_off + (unsigned long)y * c->xor_stride;
        for (x = 0; x < c->w; x++) {
            int a = (c->comp == 3)
                ? ico_mask_get(rd32(xrow + x * 4), c->sa, c->ba)
                : (int)xrow[x * 4 + 3];
            if (a == 0)
                seen_a0 = 1;
            else if (a == 255)
                seen_a255 = 1;
            else
                seen_amid = 1;
            if (seen_amid || (seen_a0 && seen_a255)) {
                c->alpha_mode = ICO_ALPHA_BGRA;
                return 1;
            }
        }
    }

    /* All alphas are identically 0 or identically 255: honour AND mask if set. */
    c->alpha_mode = any_and ? ICO_ALPHA_AND : ICO_ALPHA_OPAQUE;
    return any_and ? 1 : 0;
}

static void ico_decode_rows(const struct ico_dib_ctx *c, struct image *im)
{
    long y, x;

    for (y = 0; y < c->h; y++) {
        long sy = c->top_down ? y : (c->h - 1 - y);
        const uint8_t *xrow = c->p + c->pix_off + (unsigned long)sy * c->xor_stride;
        const uint8_t *arow = c->has_and
            ? (c->p + c->and_off + (unsigned long)sy * c->and_stride)
            : 0;
        uint32_t *dst = im->px + (long)y * c->w;
        uint8_t *da = im->alpha ? im->alpha + (long)y * c->w : 0;

        for (x = 0; x < c->w; x++) {
            int a = 255;

            if (c->bpp == 1) {
                unsigned idx = (xrow[x >> 3] >> (7 - (x & 7))) & 1u;
                dst[x] = c->pal[idx];
            } else if (c->bpp == 4) {
                unsigned idx = (x & 1) ? (xrow[x >> 1] & 0x0fu)
                                       : ((xrow[x >> 1] >> 4) & 0x0fu);
                dst[x] = c->pal[idx];
            } else if (c->bpp == 8) {
                dst[x] = c->pal[xrow[x]];
            } else if (c->bpp == 24) {
                dst[x] = img__rgb(xrow[x * 3 + 2], xrow[x * 3 + 1], xrow[x * 3]);
            } else if (c->comp == 3) {
                unsigned long v = rd32(xrow + x * 4);
                dst[x] = img__rgb(ico_mask_get(v, c->sr, c->br),
                                  ico_mask_get(v, c->sg, c->bg),
                                  ico_mask_get(v, c->sb, c->bb));
                a = ico_mask_get(v, c->sa, c->ba);
            } else {
                dst[x] = img__rgb(xrow[x * 4 + 2], xrow[x * 4 + 1], xrow[x * 4]);
                a = xrow[x * 4 + 3];
            }

            if (da) {
                if (c->alpha_mode == ICO_ALPHA_AND && arow) {
                    int and_bit = (arow[x >> 3] >> (7 - (x & 7))) & 1;
                    da[x] = and_bit ? 0 : 255;
                } else {
                    da[x] = (uint8_t)a;
                }
            }
        }
    }
}

static int ico_decode_dib(const struct ico_entry *ent, struct image *out)
{
    struct ico_dib_ctx c;
    struct image im;
    unsigned long bi_size, clr_used, pal_off, ncolors, i;
    int want_alpha, rc;

    memset(&c, 0, sizeof(c));
    memset(&im, 0, sizeof(im));
    c.p = ent->data;
    c.sz = ent->len;

    rc = ico_dib_dims(c.p, c.sz, ent->dir_h, &c.w, &c.h,
                      &c.top_down, &c.has_and, &bi_size);
    if (rc != IMG_OK)
        return rc;

    c.bpp = (int)rd16(c.p + 14);
    c.comp = rd32(c.p + 16);
    clr_used = rd32(c.p + 32);

    if (c.bpp != 1 && c.bpp != 4 && c.bpp != 8 && c.bpp != 24 && c.bpp != 32)
        return IMG_ERR_UNSUPPORTED;
    if (c.comp != 0 && !(c.comp == 3 && c.bpp == 32))
        return IMG_ERR_UNSUPPORTED;

    pal_off = bi_size;
    if (c.comp == 3) {
        unsigned long mr, mg, mb, ma;
        unsigned long mask_off = 40UL;
        if (c.sz < mask_off + 12UL)
            return IMG_ERR_TRUNCATED;
        mr = rd32(c.p + mask_off);
        mg = rd32(c.p + mask_off + 4);
        mb = rd32(c.p + mask_off + 8);
        ma = (bi_size >= 56 && c.sz >= mask_off + 16UL)
            ? rd32(c.p + mask_off + 12) : 0;
        if ((mr | mg | mb) == 0)
            return IMG_ERR_CORRUPT;
        ico_mask_info(mr, &c.sr, &c.br);
        ico_mask_info(mg, &c.sg, &c.bg);
        ico_mask_info(mb, &c.sb, &c.bb);
        ico_mask_info(ma, &c.sa, &c.ba);
        if (bi_size == 40)
            pal_off += 12UL;
    }

    if (c.bpp <= 8) {
        ncolors = (clr_used == 0) ? (1UL << c.bpp) : clr_used;
        if (ncolors > 256)
            return IMG_ERR_CORRUPT;
        if (pal_off > c.sz || ncolors * 4UL > c.sz - pal_off)
            return IMG_ERR_TRUNCATED;
        c.pal = (uint32_t *)calloc(256, sizeof(uint32_t));
        if (!c.pal)
            return IMG_ERR_NOMEM;
        for (i = 0; i < ncolors; i++) {
            const uint8_t *q = c.p + pal_off + i * 4UL;
            c.pal[i] = img__rgb(q[2], q[1], q[0]);
        }
        c.pix_off = pal_off + ncolors * 4UL;
    } else {
        if (clr_used > 256)
            return IMG_ERR_CORRUPT;
        if (pal_off > c.sz || clr_used * 4UL > c.sz - pal_off)
            return IMG_ERR_TRUNCATED;
        c.pix_off = pal_off + clr_used * 4UL;
    }

    rc = ico_check_bounds(&c);
    if (rc != IMG_OK) {
        free(c.pal);
        return rc;
    }

    want_alpha = ico_scan_alpha(&c);
    rc = img__alloc(&im, c.w, c.h, want_alpha);
    if (rc != IMG_OK) {
        free(c.pal);
        return rc;
    }

    ico_decode_rows(&c, &im);
    free(c.pal);
    *out = im;
    return IMG_OK;
}

int ico_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct ico_entry ent;
    int rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    rc = ico_select(d, len, &ent);
    if (rc != IMG_OK)
        return rc;

    if (ent.is_png)
        return png_decode(ent.data, ent.len, out);
    return ico_decode_dib(&ent, out);
}
