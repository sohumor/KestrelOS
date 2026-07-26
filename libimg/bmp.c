/* KestrelOS libimg: Windows BMP, 24- and 32-bit uncompressed.
 *
 * Small, but occasionally what a page actually serves, and useful as a
 * ground-truth format for the test harness because it has no compression
 * to get in the way.
 *
 * Handles BITMAPINFOHEADER and later (V4/V5 are supersets), both row
 * orders, and BI_BITFIELDS masks for 32-bit files. Alpha is only honoured
 * when the file actually varies it: a great many 32-bit BMPs leave the
 * fourth byte zero, and treating that as "fully transparent" would make
 * the image vanish.
 */

#include <stdlib.h>
#include <string.h>

#include "img.h"

static unsigned rd16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32(const uint8_t *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* Position and width of a colour mask, so an arbitrary BI_BITFIELDS
 * layout can be normalised to 8 bits. */
static void mask_info(unsigned long m, int *shift, int *bits)
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

static int mask_get(unsigned long v, int shift, int bits)
{
    unsigned long x;

    if (bits <= 0)
        return 0;
    x = (v >> shift) & ((1UL << bits) - 1);
    if (bits >= 8)
        return (int)(x >> (bits - 8));
    /* Spread a short field over the full range: 5 bits of 31 -> 255. */
    return (int)((x * 255UL) / ((1UL << bits) - 1));
}

int bmp_decode(const void *data, unsigned long len, struct image *out)
{
    const uint8_t *d = (const uint8_t *)data;
    struct image im;
    unsigned long hdr, off, comp, stride;
    unsigned long mr = 0, mg = 0, mb = 0, ma = 0;
    long w, h, y;
    int bpp, top_down = 0, rc;
    int sr = 0, br = 0, sg = 0, bg = 0, sb = 0, bb = 0, sa = 0, ba = 0;
    int any_alpha = 0;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(&im, 0, sizeof(im));
    if (!d || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;
    if (len < 54)
        return IMG_ERR_TRUNCATED;
    if (d[0] != 'B' || d[1] != 'M')
        return IMG_ERR_FORMAT;

    off = rd32(d + 10);
    hdr = rd32(d + 14);
    if (hdr < 40)
        return IMG_ERR_UNSUPPORTED;       /* BITMAPCOREHEADER and friends */
    if (hdr > len - 14)
        return IMG_ERR_TRUNCATED;

    w = (long)(int32_t)(uint32_t)rd32(d + 18);
    h = (long)(int32_t)(uint32_t)rd32(d + 22);
    bpp = (int)rd16(d + 28);
    comp = rd32(d + 30);

    if (h < 0) {
        top_down = 1;
        h = -h;
    }
    if (w <= 0 || h <= 0)
        return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM ||
        (unsigned long)w * (unsigned long)h > IMG_MAX_PIXELS)
        return IMG_ERR_TOOBIG;
    if (bpp != 24 && bpp != 32)
        return IMG_ERR_UNSUPPORTED;
    if (comp != 0 && !(comp == 3 && bpp == 32))
        return IMG_ERR_UNSUPPORTED;

    if (comp == 3) {
        /* The three masks sit immediately after a 40-byte header; V4 and
         * V5 headers additionally carry an alpha mask at header offset 52. */
        if (len < 66)
            return IMG_ERR_TRUNCATED;
        mr = rd32(d + 54);
        mg = rd32(d + 58);
        mb = rd32(d + 62);
        ma = (hdr >= 56 && len >= 70) ? rd32(d + 66) : 0;
        if ((mr | mg | mb) == 0)
            return IMG_ERR_CORRUPT;
        mask_info(mr, &sr, &br);
        mask_info(mg, &sg, &bg);
        mask_info(mb, &sb, &bb);
        mask_info(ma, &sa, &ba);
    }

    stride = (((unsigned long)w * (unsigned)bpp + 31) / 32) * 4;
    {
        unsigned long rowbytes = ((unsigned long)w * (unsigned)bpp + 7) / 8;
        unsigned long need;

        if (off >= len)
            return IMG_ERR_TRUNCATED;
        /* The final row may legitimately lack its padding. */
        need = (unsigned long)(h - 1) * stride + rowbytes;
        if (need > len - off)
            return IMG_ERR_TRUNCATED;
    }

    /* Decide up front whether the alpha byte carries information, so the
     * image does not end up entirely transparent because of padding. */
    if (bpp == 32 && !(comp == 3 && ba == 0)) {
        int seen0 = 0, seen_other = 0;

        for (y = 0; y < h && !(seen0 && seen_other); y++) {
            const uint8_t *row = d + off + (unsigned long)y * stride;
            long i;

            for (i = 0; i < w; i++) {
                int a = (comp == 3) ? mask_get(rd32(row + i * 4), sa, ba)
                                    : row[i * 4 + 3];
                if (a == 0)
                    seen0 = 1;
                else
                    seen_other = 1;
                if (seen0 && seen_other)
                    break;
            }
        }
        any_alpha = seen0 && seen_other;
    }

    rc = img__alloc(&im, w, h, any_alpha);
    if (rc != IMG_OK)
        return rc;

    for (y = 0; y < h; y++) {
        /* Rows are stored bottom-up unless the height was negative. */
        const uint8_t *row = d + off +
                             (unsigned long)(top_down ? y : h - 1 - y) * stride;
        uint32_t *dst = im.px + (long)y * w;
        uint8_t *da = im.alpha ? im.alpha + (long)y * w : 0;
        long x;

        for (x = 0; x < w; x++) {
            if (bpp == 24) {
                dst[x] = img__rgb(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
            } else if (comp == 3) {
                unsigned long v = rd32(row + x * 4);
                dst[x] = img__rgb(mask_get(v, sr, br), mask_get(v, sg, bg),
                                  mask_get(v, sb, bb));
                if (da)
                    da[x] = (uint8_t)mask_get(v, sa, ba);
            } else {
                dst[x] = img__rgb(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
                if (da)
                    da[x] = row[x * 4 + 3];
            }
        }
    }

    *out = im;
    return IMG_OK;
}
