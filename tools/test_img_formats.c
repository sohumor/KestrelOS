/* KestrelOS libimg multi-format standalone test suite (ICO/CUR, SVG, QOI, Netpbm).
 *
 * Pure C11 with zero external files or POSIX dependencies.
 * Verifies img_sniff, img_format_name, img_probe, img_decode, and direct
 * per-format entrypoints across valid, edge-case, truncated, corrupt, and
 * oversized inputs.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

static int n_pass;
static int n_fail;

static void check(int cond, const char *name, const char *fmt, ...)
{
    va_list ap;

    if (cond) {
        n_pass++;
        return;
    }
    n_fail++;
    printf("  FAIL %-28s ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

/* ---------------------------------------------------------- test helpers */

static uint32_t test_crc32(const uint8_t *d, unsigned long n)
{
    uint32_t c = 0xffffffffu;
    unsigned long i;
    int b;

    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (b = 0; b < 8; b++)
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xffffffffu;
}

static uint32_t test_adler32(const uint8_t *d, unsigned long n)
{
    uint32_t s1 = 1, s2 = 0;
    unsigned long i;

    for (i = 0; i < n; i++) {
        s1 = (s1 + d[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

static unsigned long append_png_chunk(uint8_t *out, const char *type4,
                                      const uint8_t *body, unsigned long blen)
{
    wr32be(out, (uint32_t)blen);
    memcpy(out + 4, type4, 4);
    if (blen > 0)
        memcpy(out + 8, body, blen);
    wr32be(out + 8 + blen, test_crc32(out + 4, 4 + blen));
    return 12 + blen;
}

/* Build a valid uncompressed RGBA PNG in a heap buffer. */
static uint8_t *make_rgba_png(int w, int h, const uint32_t *argb,
                              unsigned long *out_len)
{
    unsigned long raw_len = (unsigned long)h * (1UL + (unsigned long)w * 4UL);
    unsigned long zlen = 2UL + 5UL + raw_len + 4UL;
    unsigned long cap = 8UL + 25UL + 12UL + zlen + 12UL;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uint8_t *zbuf = (uint8_t *)malloc(zlen);
    uint8_t *png = (uint8_t *)malloc(cap);
    uint8_t ihdr[13];
    unsigned long pos = 0, rpos = 0;
    int x, y;
    static const uint8_t sig[8] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
    };

    if (!raw || !zbuf || !png || raw_len > 65535UL) {
        free(raw); free(zbuf); free(png);
        return 0;
    }
    for (y = 0; y < h; y++) {
        raw[rpos++] = 0; /* filter None */
        for (x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            raw[rpos++] = (uint8_t)((c >> 16) & 0xffu);
            raw[rpos++] = (uint8_t)((c >> 8) & 0xffu);
            raw[rpos++] = (uint8_t)(c & 0xffu);
            raw[rpos++] = (uint8_t)((c >> 24) & 0xffu);
        }
    }

    /* Stored zlib stream: CMF=0x78, FLG=0x01, BFINAL|BTYPE=0x01, LEN, NLEN */
    zbuf[0] = 0x78;
    zbuf[1] = 0x01;
    zbuf[2] = 0x01;
    wr16le(zbuf + 3, (uint16_t)raw_len);
    wr16le(zbuf + 5, (uint16_t)(~raw_len));
    memcpy(zbuf + 7, raw, raw_len);
    wr32be(zbuf + 7 + raw_len, test_adler32(raw, raw_len));

    memcpy(png, sig, 8);
    pos = 8;
    wr32be(ihdr, (uint32_t)w);
    wr32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    pos += append_png_chunk(png + pos, "IHDR", ihdr, 13);
    pos += append_png_chunk(png + pos, "IDAT", zbuf, zlen);
    pos += append_png_chunk(png + pos, "IEND", 0, 0);

    free(raw);
    free(zbuf);
    *out_len = pos;
    return png;
}

/* Encode an RGBA/RGB image to QOI exercising all 6 opcodes. */
static uint8_t *make_qoi(int w, int h, int channels, int cs,
                         const uint32_t *argb, unsigned long *out_len)
{
    unsigned long npix = (unsigned long)w * (unsigned long)h;
    unsigned long cap = 14UL + npix * 5UL + 8UL;
    uint8_t *buf = (uint8_t *)malloc(cap);
    uint32_t *idx = (uint32_t *)calloc(64, sizeof(uint32_t));
    uint8_t pr = 0, pg = 0, pb = 0, pa = 255;
    unsigned long pos = 14, i;
    int run = 0;
    static const uint8_t end_m[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };

    if (!buf || !idx) {
        free(buf); free(idx);
        return 0;
    }
    memcpy(buf, "qoif", 4);
    wr32be(buf + 4, (uint32_t)w);
    wr32be(buf + 8, (uint32_t)h);
    buf[12] = (uint8_t)channels;
    buf[13] = (uint8_t)cs;

    for (i = 0; i < npix; i++) {
        uint32_t c = argb[i];
        uint8_t r = (uint8_t)((c >> 16) & 0xffu);
        uint8_t g = (uint8_t)((c >> 8) & 0xffu);
        uint8_t b = (uint8_t)(c & 0xffu);
        uint8_t a = (channels == 4) ? (uint8_t)((c >> 24) & 0xffu) : 255u;
        uint32_t packed = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                          ((uint32_t)g << 8) | b;

        if (r == pr && g == pg && b == pb && a == pa) {
            run++;
            if (run == 62 || i + 1 == npix) {
                buf[pos++] = (uint8_t)(0xc0u | (unsigned)(run - 1));
                run = 0;
            }
        } else {
            unsigned hidx;
            if (run > 0) {
                buf[pos++] = (uint8_t)(0xc0u | (unsigned)(run - 1));
                run = 0;
            }
            hidx = ((unsigned)r * 3u + (unsigned)g * 5u +
                    (unsigned)b * 7u + (unsigned)a * 11u) & 63u;
            if (idx[hidx] == packed && packed != 0) {
                buf[pos++] = (uint8_t)hidx;
            } else {
                idx[hidx] = packed;
                if (a == pa) {
                    int8_t dr = (int8_t)(r - pr);
                    int8_t dg = (int8_t)(g - pg);
                    int8_t db = (int8_t)(b - pb);
                    int8_t dr_g = (int8_t)(dr - dg);
                    int8_t db_g = (int8_t)(db - dg);
                    if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 &&
                        db >= -2 && db <= 1) {
                        buf[pos++] = (uint8_t)(0x40u |
                            ((unsigned)(dr + 2) << 4) |
                            ((unsigned)(dg + 2) << 2) |
                            (unsigned)(db + 2));
                    } else if (dg >= -32 && dg <= 31 &&
                               dr_g >= -8 && dr_g <= 7 &&
                               db_g >= -8 && db_g <= 7) {
                        buf[pos++] = (uint8_t)(0x80u | (unsigned)(dg + 32));
                        buf[pos++] = (uint8_t)(((unsigned)(dr_g + 8) << 4) |
                                               (unsigned)(db_g + 8));
                    } else {
                        buf[pos++] = 0xfe;
                        buf[pos++] = r;
                        buf[pos++] = g;
                        buf[pos++] = b;
                    }
                } else {
                    buf[pos++] = 0xff;
                    buf[pos++] = r;
                    buf[pos++] = g;
                    buf[pos++] = b;
                    buf[pos++] = a;
                }
            }
            pr = r; pg = g; pb = b; pa = a;
        }
    }
    memcpy(buf + pos, end_m, 8);
    pos += 8;
    free(idx);
    *out_len = pos;
    return buf;
}

/* ---------------------------------------------------------- 1. sniff tests */

static void test_sniff_and_names(void)
{
    static const uint8_t ico_hdr[6] = { 0, 0, 1, 0, 1, 0 };
    static const uint8_t cur_hdr[6] = { 0, 0, 2, 0, 1, 0 };
    static const uint8_t bad_ico[6] = { 0, 0, 3, 0, 1, 0 };
    static const uint8_t qoi_hdr[14] = { 'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0 };
    static const char svg_bom[] = "\xef\xbb\xbf  \n<svg width='8' height='8'/>";
    static const char svg_xml[] = "<?xml version=\"1.0\"?>\n<!-- hi -->\n<svg>";
    static const char html_xml[] = "<?xml version=\"1.0\"?><html><svg/></html>";
    char *long_xml;

    check(strcmp(img_format_name(IMG_FMT_ICO), "ICO") == 0, "name_ico", "got %s", img_format_name(IMG_FMT_ICO));
    check(strcmp(img_format_name(IMG_FMT_SVG), "SVG") == 0, "name_svg", "got %s", img_format_name(IMG_FMT_SVG));
    check(strcmp(img_format_name(IMG_FMT_QOI), "QOI") == 0, "name_qoi", "got %s", img_format_name(IMG_FMT_QOI));
    check(strcmp(img_format_name(IMG_FMT_PNM), "PNM") == 0, "name_pnm", "got %s", img_format_name(IMG_FMT_PNM));
    check(strcmp(img_format_name(IMG_FMT_UNKNOWN), "unknown") == 0, "name_unk", "bad");

    check(img_sniff(ico_hdr, sizeof(ico_hdr)) == IMG_FMT_ICO, "sniff_ico", "bad");
    check(img_sniff(cur_hdr, sizeof(cur_hdr)) == IMG_FMT_ICO, "sniff_cur", "bad");
    check(img_sniff(bad_ico, sizeof(bad_ico)) == IMG_FMT_UNKNOWN, "sniff_bad_ico", "bad");
    check(img_sniff(qoi_hdr, sizeof(qoi_hdr)) == IMG_FMT_QOI, "sniff_qoi", "bad");
    check(img_sniff("P1\n1 1\n0\n", 9) == IMG_FMT_PNM, "sniff_p1", "bad");
    check(img_sniff("P2 1 1 255 0", 12) == IMG_FMT_PNM, "sniff_p2", "bad");
    check(img_sniff("P3\t1 1 255 0 0 0", 16) == IMG_FMT_PNM, "sniff_p3", "bad");
    check(img_sniff("P4\r1 1\n\x00", 8) == IMG_FMT_PNM, "sniff_p4", "bad");
    check(img_sniff("P5#c\n1 1 255\n\x00", 14) == IMG_FMT_PNM, "sniff_p5_cmt", "bad");
    check(img_sniff("P6\n1 1 255\n\x00\x00\x00", 14) == IMG_FMT_PNM, "sniff_p6", "bad");
    check(img_sniff("P7\n1 1\n", 7) == IMG_FMT_UNKNOWN, "sniff_p7", "bad");
    check(img_sniff("P10\n", 4) == IMG_FMT_UNKNOWN, "sniff_p10", "bad");

    check(img_sniff(svg_bom, sizeof(svg_bom) - 1) == IMG_FMT_SVG, "sniff_svg_bom", "bad");
    check(img_sniff(svg_xml, sizeof(svg_xml) - 1) == IMG_FMT_SVG, "sniff_svg_xml", "bad");
    check(img_sniff(html_xml, sizeof(html_xml) - 1) == IMG_FMT_UNKNOWN, "sniff_html_xml", "bad");
    check(img_sniff("<svga>", 6) == IMG_FMT_UNKNOWN, "sniff_svga", "bad");
    check(img_sniff(0, 0) == IMG_FMT_UNKNOWN, "sniff_null", "bad");

    long_xml = (char *)malloc(600);
    if (long_xml) {
        memset(long_xml, ' ', 600);
        memcpy(long_xml, "<?xml version=\"1.0\"?>", 21);
        memcpy(long_xml + 490, "<svg>", 5);
        check(img_sniff(long_xml, 600) == IMG_FMT_SVG, "sniff_svg_within_512", "bad");
        memset(long_xml + 490, ' ', 10);
        memcpy(long_xml + 520, "<svg>", 5);
        check(img_sniff(long_xml, 600) == IMG_FMT_UNKNOWN, "sniff_svg_past_512", "bad");
        free(long_xml);
    }
}

/* ---------------------------------------------------------- 2. ICO / CUR tests */

static void test_ico(void)
{
    /* 2a. PNG-in-ICO + multi-entry selection (2x2 PNG beats 1x1 1bpp DIB). */
    {
        static const uint32_t px[4] = {
            0xffff0000u, 0x8000ff00u,
            0x000000ffu, 0xffffffffu
        };
        unsigned long png_len = 0;
        uint8_t *png = make_rgba_png(2, 2, px, &png_len);
        /* Entry 0: 1x1 1bpp DIB (header 40 + 8 pal + 4 xor + 4 and = 56 bytes) */
        unsigned long dib_len = 56;
        unsigned long total = 6 + 32 + dib_len + png_len;
        uint8_t *ico = (uint8_t *)calloc(1, total);
        struct img_info info;
        struct image im;
        int rc;

        check(png != 0 && ico != 0, "ico_png_alloc", "oom");
        if (png && ico) {
            wr16le(ico + 0, 0);
            wr16le(ico + 2, 1); /* ICO */
            wr16le(ico + 4, 2); /* 2 entries */

            /* Entry 0: 1x1 1bpp at offset 38 */
            ico[6 + 0] = 1; ico[6 + 1] = 1; ico[6 + 2] = 2;
            wr16le(ico + 6 + 4, 1);
            wr16le(ico + 6 + 6, 1);
            wr32le(ico + 6 + 8, (uint32_t)dib_len);
            wr32le(ico + 6 + 12, 38);
            wr32le(ico + 38, 40);
            wr32le(ico + 38 + 4, 1);
            wr32le(ico + 38 + 8, 2);
            wr16le(ico + 38 + 12, 1);
            wr16le(ico + 38 + 14, 1);

            /* Entry 1: 2x2 PNG at offset 38 + 56 = 94 */
            ico[22 + 0] = 2; ico[22 + 1] = 2;
            wr16le(ico + 22 + 4, 1);
            wr16le(ico + 22 + 6, 32);
            wr32le(ico + 22 + 8, (uint32_t)png_len);
            wr32le(ico + 22 + 12, (uint32_t)(38 + dib_len));
            memcpy(ico + 38 + dib_len, png, png_len);

            rc = img_probe(ico, total, &info);
            check(rc == IMG_OK && info.fmt == IMG_FMT_ICO &&
                  info.w == 2 && info.h == 2 && info.has_alpha == 1,
                  "ico_png_probe", "rc=%d w=%d h=%d a=%d",
                  rc, info.w, info.h, info.has_alpha);

            rc = img_decode(ico, total, &im);
            check(rc == IMG_OK && im.w == 2 && im.h == 2 && im.has_alpha &&
                  im.px[0] == 0xff0000u && im.alpha[0] == 255 &&
                  im.px[1] == 0x00ff00u && im.alpha[1] == 128 &&
                  im.alpha[2] == 0 && im.px[3] == 0xffffffu,
                  "ico_png_decode", "rc=%d px0=%06x a1=%u",
                  rc, im.px ? im.px[0] : 0, im.alpha ? im.alpha[1] : 999);
            img_free(&im);
        }
        free(png);
        free(ico);
    }

    /* 2b. Directory width/height 0 => 256x256 preference over 32x32. */
    {
        static const uint32_t px16[4] = { 0xff112233u, 0xff445566u, 0xff778899u, 0xffaabbccu };
        uint32_t *px256 = (uint32_t *)malloc(256UL * 256UL * 4UL);
        unsigned long dib16_sz = 40 + 4 * 16 + 2 * 4;
        /* 256x256 1bpp DIB: 40 hdr + 8 pal + 256*32 xor + 256*32 and = 16432 */
        unsigned long dib256_sz = 40 + 8 + 256UL * 32UL + 256UL * 32UL;
        unsigned long total = 6 + 32 + dib16_sz + dib256_sz;
        uint8_t *ico = (uint8_t *)calloc(1, total);
        struct img_info info;
        struct image im;
        int rc;
        (void)px16;

        check(px256 != 0 && ico != 0, "ico_256_alloc", "oom");
        if (px256 && ico) {
            unsigned long off0 = 38;
            unsigned long off1 = 38 + dib16_sz;
            wr16le(ico + 2, 1);
            wr16le(ico + 4, 2);

            ico[6] = 2; ico[7] = 2;
            wr16le(ico + 10, 1); wr16le(ico + 12, 32);
            wr32le(ico + 14, (uint32_t)dib16_sz);
            wr32le(ico + 18, (uint32_t)off0);
            wr32le(ico + off0, 40);
            wr32le(ico + off0 + 4, 2);
            wr32le(ico + off0 + 8, 4);
            wr16le(ico + off0 + 12, 1);
            wr16le(ico + off0 + 14, 32);

            ico[22] = 0; ico[23] = 0; /* 256x256! */
            wr16le(ico + 26, 1); wr16le(ico + 28, 1);
            wr32le(ico + 30, (uint32_t)dib256_sz);
            wr32le(ico + 34, (uint32_t)off1);
            wr32le(ico + off1, 40);
            wr32le(ico + off1 + 4, 256);
            wr32le(ico + off1 + 8, 512);
            wr16le(ico + off1 + 12, 1);
            wr16le(ico + off1 + 14, 1);
            /* Palette[0]=red, Palette[1]=blue */
            ico[off1 + 40 + 2] = 0xff;
            ico[off1 + 44 + 0] = 0xff;
            /* Set bit 0 (top-left in bottom-up = row 255 byte 0 bit 7) to 1 */
            ico[off1 + 48 + 255 * 32] = 0x80;

            rc = img_probe(ico, total, &info);
            check(rc == IMG_OK && info.w == 256 && info.h == 256,
                  "ico_256_probe", "rc=%d w=%d h=%d", rc, info.w, info.h);
            rc = img_decode(ico, total, &im);
            check(rc == IMG_OK && im.w == 256 && im.h == 256 &&
                  im.px[0] == 0x0000ffu && im.px[1] == 0xff0000u,
                  "ico_256_decode", "rc=%d px0=%06x px1=%06x",
                  rc, im.px ? im.px[0] : 0, im.px ? im.px[1] : 0);
            img_free(&im);
        }
        free(px256);
        free(ico);
    }

    /* 2c. 32-bit BGRA DIB-in-ICO (top-down biHeight = -4) + CUR (type 2). */
    {
        unsigned long sz = 6 + 16 + 40 + 2 * 8 + 2 * 4;
        uint8_t *cur = (uint8_t *)calloc(1, sz);
        uint8_t *dib = cur + 22;
        struct image im;
        int rc;

        check(cur != 0, "cur_alloc", "oom");
        if (cur) {
            wr16le(cur + 2, 2); /* CUR */
            wr16le(cur + 4, 1);
            cur[6] = 2; cur[7] = 2;
            wr16le(cur + 10, 1); /* hotspot X */
            wr16le(cur + 12, 1); /* hotspot Y (NOT bpp!) */
            wr32le(cur + 14, (uint32_t)(sz - 22));
            wr32le(cur + 18, 22);

            wr32le(dib, 40);
            wr32le(dib + 4, 2);
            wr32le(dib + 8, (uint32_t)(-4)); /* top-down 2*2 */
            wr16le(dib + 12, 1);
            wr16le(dib + 14, 32);
            /* Row 0 (top): (10,20,30,a=255), (40,50,60,a=100) */
            dib[40 + 0] = 30; dib[40 + 1] = 20; dib[40 + 2] = 10; dib[40 + 3] = 255;
            dib[40 + 4] = 60; dib[40 + 5] = 50; dib[40 + 6] = 40; dib[40 + 7] = 100;
            /* Row 1 (bottom): (1,2,3,a=0), (7,8,9,a=255) */
            dib[48 + 0] = 3; dib[48 + 1] = 2; dib[48 + 2] = 1; dib[48 + 3] = 0;
            dib[48 + 4] = 9; dib[48 + 5] = 8; dib[48 + 6] = 7; dib[48 + 7] = 255;

            rc = img_decode(cur, sz, &im);
            check(rc == IMG_OK && im.w == 2 && im.h == 2 && im.has_alpha &&
                  im.px[0] == img__rgb(10, 20, 30) && im.alpha[0] == 255 &&
                  im.px[1] == img__rgb(40, 50, 60) && im.alpha[1] == 100 &&
                  im.alpha[2] == 0 && im.px[3] == img__rgb(7, 8, 9),
                  "cur_32_topdown", "rc=%d px0=%06x a1=%u",
                  rc, im.px ? im.px[0] : 0, im.alpha ? im.alpha[1] : 999);
            img_free(&im);
            free(cur);
        }
    }

    /* 2d. 8-bit paletted DIB-in-ICO with 1-bit AND transparency (bottom-up). */
    {
        /* 4x2 8bpp: 40 hdr + 4*4 pal + 2*4 xor + 2*4 and = 72 */
        unsigned long sz = 6 + 16 + 40 + 16 + 8 + 8;
        uint8_t *ico = (uint8_t *)calloc(1, sz);
        uint8_t *dib = ico + 22;
        struct image im;
        int rc;

        if (ico) {
            wr16le(ico + 2, 1); wr16le(ico + 4, 1);
            ico[6] = 4; ico[7] = 2; ico[8] = 4;
            wr16le(ico + 10, 1); wr16le(ico + 12, 8);
            wr32le(ico + 14, (uint32_t)(sz - 22));
            wr32le(ico + 18, 22);

            wr32le(dib, 40);
            wr32le(dib + 4, 4);
            wr32le(dib + 8, 4); /* 2*2 bottom-up */
            wr16le(dib + 12, 1);
            wr16le(dib + 14, 8);
            wr32le(dib + 32, 4); /* clrUsed = 4 */

            /* Palette: 0=black, 1=red, 2=green, 3=blue */
            dib[40 + 4 + 2] = 255;
            dib[40 + 8 + 1] = 255;
            dib[40 + 12 + 0] = 255;

            /* XOR bottom row (y=1): [3, 2, 1, 0] */
            dib[56 + 0] = 3; dib[56 + 1] = 2; dib[56 + 2] = 1; dib[56 + 3] = 0;
            /* XOR top row (y=0): [1, 2, 3, 0] */
            dib[60 + 0] = 1; dib[60 + 1] = 2; dib[60 + 2] = 3; dib[60 + 3] = 0;
            /* AND bottom row (y=1): bit 3 (x=3) transparent => 0x10 */
            dib[64] = 0x10;
            /* AND top row (y=0): bit 0 (x=0) transparent => 0x80 */
            dib[68] = 0x80;

            rc = img_decode(ico, sz, &im);
            check(rc == IMG_OK && im.w == 4 && im.h == 2 && im.has_alpha &&
                  im.alpha[0] == 0 && im.px[1] == 0x00ff00u && im.alpha[1] == 255 &&
                  im.px[2] == 0x0000ffu && im.px[4] == 0x0000ffu && im.alpha[7] == 0,
                  "ico_8bpp_and", "rc=%d a0=%u px1=%06x a7=%u",
                  rc, im.alpha ? im.alpha[0] : 9, im.px ? im.px[1] : 0,
                  im.alpha ? im.alpha[7] : 9);
            img_free(&im);
            free(ico);
        }
    }

    /* 2e. 4-bit paletted + 24-bit padded odd-width + 1-bit paletted ICO. */
    {
        /* 4-bit 2x2: 40 + 16*4 + 2*4 + 2*4 = 120 */
        unsigned long sz4 = 6 + 16 + 120;
        uint8_t *ico4 = (uint8_t *)calloc(1, sz4);
        uint8_t *d4 = ico4 + 22;
        struct image im;
        int rc;

        if (ico4) {
            wr16le(ico4 + 2, 1); wr16le(ico4 + 4, 1);
            ico4[6] = 2; ico4[7] = 2;
            wr16le(ico4 + 10, 1); wr16le(ico4 + 12, 4);
            wr32le(ico4 + 14, 120); wr32le(ico4 + 18, 22);
            wr32le(d4, 40); wr32le(d4 + 4, 2); wr32le(d4 + 8, 4);
            wr16le(d4 + 12, 1); wr16le(d4 + 14, 4);
            d4[40 + 1 * 4 + 2] = 0xff; /* idx 1 = red */
            d4[40 + 2 * 4 + 1] = 0xff; /* idx 2 = green */
            d4[104] = 0x21;            /* y=1: x=0 -> 2(green), x=1 -> 1(red) */
            d4[108] = 0x12;            /* y=0: x=0 -> 1(red), x=1 -> 2(green) */
            d4[112] = 0x40;            /* y=1, x=1 transparent */

            rc = img_decode(ico4, sz4, &im);
            check(rc == IMG_OK && im.px[0] == 0xff0000u && im.px[1] == 0x00ff00u &&
                  im.px[2] == 0x00ff00u && im.alpha[2] == 255 && im.alpha[3] == 0,
                  "ico_4bpp_and", "rc=%d", rc);
            img_free(&im);
            free(ico4);
        }

        /* 24-bit 3x1 (stride 12 bytes) + 4 byte AND = 56 bytes DIB */
        {
            unsigned long sz24 = 6 + 16 + 40 + 12 + 4;
            uint8_t *ico24 = (uint8_t *)calloc(1, sz24);
            uint8_t *d24 = ico24 + 22;
            if (ico24) {
                wr16le(ico24 + 2, 1); wr16le(ico24 + 4, 1);
                ico24[6] = 3; ico24[7] = 1;
                wr16le(ico24 + 10, 1); wr16le(ico24 + 12, 24);
                wr32le(ico24 + 14, 56); wr32le(ico24 + 18, 22);
                wr32le(d24, 40); wr32le(d24 + 4, 3); wr32le(d24 + 8, 2);
                wr16le(d24 + 12, 1); wr16le(d24 + 14, 24);
                d24[40 + 2] = 0xaa; /* pixel 0 R */
                d24[40 + 4] = 0xbb; /* pixel 1 G */
                d24[40 + 6] = 0xcc; /* pixel 2 B */
                d24[52] = 0x20;     /* x=2 transparent */
                rc = img_decode(ico24, sz24, &im);
                check(rc == IMG_OK && im.px[0] == 0xaa0000u &&
                      im.px[1] == 0x00bb00u && im.alpha[1] == 255 &&
                      im.alpha[2] == 0, "ico_24bpp_and", "rc=%d", rc);
                img_free(&im);
                free(ico24);
            }
        }

        /* 1-bit 5x2 (padded 4-byte rows) + 32-bit XRGB (all-0 alpha + AND mask). */
        {
            unsigned long sz1 = 6 + 16 + 40 + 8 + 8 + 8;
            uint8_t *ico1 = (uint8_t *)calloc(1, sz1);
            uint8_t *d1 = ico1 + 22;
            if (ico1) {
                wr16le(ico1 + 2, 1); wr16le(ico1 + 4, 1);
                ico1[6] = 5; ico1[7] = 2;
                wr16le(ico1 + 10, 1); wr16le(ico1 + 12, 1);
                wr32le(ico1 + 14, (uint32_t)(sz1 - 22));
                wr32le(ico1 + 18, 22);
                wr32le(d1, 40); wr32le(d1 + 4, 5); wr32le(d1 + 8, 4);
                wr16le(d1 + 12, 1); wr16le(d1 + 14, 1);
                d1[40 + 0] = 0x10; d1[40 + 1] = 0x20; d1[40 + 2] = 0x30;
                d1[44 + 0] = 0xff; d1[44 + 1] = 0xff; d1[44 + 2] = 0xff;
                d1[48] = 0x80; /* y=1: x=0 => 1 */
                d1[52] = 0x08; /* y=0: x=4 => 1 */
                d1[56] = 0x40; /* y=1: x=1 transparent */
                d1[60] = 0x80; /* y=0: x=0 transparent */
                rc = ico_decode(ico1, sz1, &im);
                check(rc == IMG_OK && im.w == 5 && im.h == 2 &&
                      im.alpha[0] == 0 && im.px[4] == 0xffffffu &&
                      im.px[5] == 0xffffffu && im.alpha[6] == 0 &&
                      im.px[7] == img__rgb(0x30, 0x20, 0x10),
                      "ico_1bpp_5x2", "rc=%d", rc);
                img_free(&im);
                free(ico1);
            }

            /* 32bpp XRGB (alpha bytes all 0, AND mask punches x=0). */
            {
                unsigned long szx = 6 + 16 + 40 + 8 + 4;
                uint8_t *icox = (uint8_t *)calloc(1, szx);
                uint8_t *dx = icox + 22;
                if (icox) {
                    wr16le(icox + 2, 1); wr16le(icox + 4, 1);
                    icox[6] = 2; icox[7] = 1;
                    wr16le(icox + 10, 1); wr16le(icox + 12, 32);
                    wr32le(icox + 14, (uint32_t)(szx - 22));
                    wr32le(icox + 18, 22);
                    wr32le(dx, 40); wr32le(dx + 4, 2); wr32le(dx + 8, 2);
                    wr16le(dx + 12, 1); wr16le(dx + 14, 32);
                    dx[40 + 2] = 0xee; /* x=0 R, a=0 */
                    dx[44 + 1] = 0xdd; /* x=1 G, a=0 */
                    dx[48] = 0x80;     /* x=0 transparent via AND */
                    rc = ico_decode(icox, szx, &im);
                    check(rc == IMG_OK && im.has_alpha &&
                          im.alpha[0] == 0 && im.alpha[1] == 255 &&
                          im.px[1] == 0x00dd00u,
                          "ico_32bpp_xrgb_and", "rc=%d", rc);
                    img_free(&im);
                    free(icox);
                }
            }
        }
    }
}

/* ---------------------------------------------------------- 3. SVG tests */

static void test_svg(void)
{
    struct img_info info;
    struct image im;
    int rc;

    /* 3a. Sizing, clamping, percentages, viewBox aspect. */
    {
        static const char s1[] = "<svg viewBox=\"0 0 200 100\"/>";
        static const char s2[] = "<svg width=\"100%\" height=\"50%\" viewBox=\"0 0 100 200\"/>";
        static const char s3[] = "<svg width=\"5000\" height=\"0.1\"/>";
        static const char s4[] = "<svg/>";

        rc = img_probe(s1, sizeof(s1) - 1, &info);
        check(rc == IMG_OK && info.fmt == IMG_FMT_SVG && info.w == 128 && info.h == 64,
              "svg_probe_vb_wide", "rc=%d %dx%d", rc, info.w, info.h);

        rc = img_probe(s2, sizeof(s2) - 1, &info);
        check(rc == IMG_OK && info.w == 64 && info.h == 128,
              "svg_probe_pct_tall", "rc=%d %dx%d", rc, info.w, info.h);

        rc = img_probe(s3, sizeof(s3) - 1, &info);
        check(rc == IMG_OK && info.w == 2048 && info.h == 1,
              "svg_probe_clamp", "rc=%d %dx%d", rc, info.w, info.h);

        rc = img_probe(s4, sizeof(s4) - 1, &info);
        check(rc == IMG_OK && info.w == 128 && info.h == 128,
              "svg_probe_default", "rc=%d %dx%d", rc, info.w, info.h);
    }

    /* 3b. Shapes: rect (incl rx/ry), circle, ellipse, line, polyline, polygon. */
    {
        static const char s_shapes[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<svg width=\"64\" height=\"64\" xmlns=\"http://www.w3.org/2000/svg\">"
            "  <defs><rect x=\"0\" y=\"0\" width=\"64\" height=\"64\" fill=\"white\"/></defs>"
            "  <rect x=\"2\" y=\"2\" width=\"20\" height=\"20\" rx=\"5\" ry=\"5\" fill=\"#f00\"/>"
            "  <circle cx=\"48\" cy=\"16\" r=\"10\" fill=\"rgb(0,255,0)\" stroke=\"blue\" stroke-width=\"2\"/>"
            "  <ellipse cx=\"16\" cy=\"48\" rx=\"12\" ry=\"6\" fill=\"purple\"/>"
            "  <polygon points=\"48,34 60,58 36,58\" fill=\"orange\"/>"
            "  <line x1=\"24\" y1=\"32\" x2=\"40\" y2=\"32\" stroke=\"#112233\" stroke-width=\"2\"/>"
            "  <polyline points=\"2,30 12,30 12,36\" fill=\"none\" stroke=\"cyan\" stroke-width=\"2\"/>"
            "</svg>";

        rc = img_decode(s_shapes, sizeof(s_shapes) - 1, &im);
        check(rc == IMG_OK && im.w == 64 && im.h == 64 && im.has_alpha,
              "svg_shapes_decode", "rc=%d", rc);
        if (rc == IMG_OK) {
            /* Corner (0,0) must stay transparent (defs not drawn). */
            check(im.alpha[0] == 0, "svg_defs_skipped", "a0=%u", im.alpha[0]);
            /* Rounded rect: center (12,12) red, rounded-off corner (2,2) transparent. */
            check(im.px[12 * 64 + 12] == 0xff0000u && im.alpha[12 * 64 + 12] == 255,
                  "svg_rrect_inside", "px=%06x", im.px[12 * 64 + 12]);
            check(im.alpha[2 * 64 + 2] == 0, "svg_rrect_corner", "a=%u", im.alpha[2 * 64 + 2]);
            /* Circle: center (48,16) green, stroke rim (48,6) blue, outside (34,4) transparent. */
            check(im.px[16 * 64 + 48] == 0x00ff00u, "svg_circle_fill", "px=%06x", im.px[16 * 64 + 48]);
            check(im.px[6 * 64 + 48] == 0x0000ffu, "svg_circle_stroke", "px=%06x", im.px[6 * 64 + 48]);
            check(im.alpha[4 * 64 + 34] == 0, "svg_circle_out", "a=%u", im.alpha[4 * 64 + 34]);
            /* Ellipse: center (16,48) purple 0x800080, outside (16,38) transparent. */
            check(im.px[48 * 64 + 16] == 0x800080u, "svg_ellipse_in", "px=%06x", im.px[48 * 64 + 16]);
            check(im.alpha[38 * 64 + 16] == 0, "svg_ellipse_out", "a=%u", im.alpha[38 * 64 + 16]);
            /* Polygon: inside (48,50) orange 0xffa500, outside (36,36) transparent. */
            check(im.px[50 * 64 + 48] == 0xffa500u, "svg_poly_in", "px=%06x", im.px[50 * 64 + 48]);
            check(im.alpha[36 * 64 + 36] == 0, "svg_poly_out", "a=%u", im.alpha[36 * 64 + 36]);
            /* Line: (32,32) = 0x112233. */
            check(im.px[32 * 64 + 32] == 0x112233u, "svg_line", "px=%06x", im.px[32 * 64 + 32]);
            /* Polyline: (6,30) = cyan 0x00ffff. */
            check(im.px[30 * 64 + 6] == 0x00ffffu, "svg_polyline", "px=%06x", im.px[30 * 64 + 6]);
            img_free(&im);
        }
    }

    /* 3c. <path> with M/m, L/l, H/h, V/v, C/c, Q/q, Z/z (De Casteljau). */
    {
        static const char s_path[] =
            "<svg width=\"40\" height=\"40\">"
            "  <path d=\"M2 2 H18 V18 H2 Z\" fill=\"#00aa00\"/>"
            "  <path d=\"m22 36 q8-32 16 0 z\" fill=\"#0000ff\"/>"
            "  <path d=\"M2 36 C2 20 18 20 18 36 Z\" fill=\"#ff0000\"/>"
            "</svg>";
        rc = img_decode(s_path, sizeof(s_path) - 1, &im);
        check(rc == IMG_OK, "svg_path_decode", "rc=%d", rc);
        if (rc == IMG_OK) {
            check(im.px[10 * 40 + 10] == 0x00aa00u, "svg_path_mhvz", "px=%06x", im.px[10 * 40 + 10]);
            /* Quadratic arch apex midpoint at t=0.5: y = 0.25*36 + 0.5*4 + 0.25*36 = 20. */
            check(im.px[28 * 40 + 30] == 0x0000ffu, "svg_path_quad_in", "px=%06x", im.px[28 * 40 + 30]);
            check(im.alpha[20 * 40 + 22] == 0, "svg_path_quad_out", "a=%u", im.alpha[20 * 40 + 22]);
            /* Cubic arch: inside (10,30) red, top-left (2,20) transparent. */
            check(im.px[30 * 40 + 10] == 0xff0000u, "svg_path_cubic_in", "px=%06x", im.px[30 * 40 + 10]);
            check(im.alpha[20 * 40 + 2] == 0, "svg_path_cubic_out", "a=%u", im.alpha[20 * 40 + 2]);
            img_free(&im);
        }
    }

    /* 3d. <g> inheritance, transform translate/scale, currentColor, viewBox & alpha blend. */
    {
        static const char s_grp[] =
            "<svg width=\"40\" height=\"40\" viewBox=\"0 0 20 20\">"
            "  <g color=\"#00ff00\" fill=\"currentColor\" transform=\"translate(5, 2) scale(2)\">"
            "    <rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
            "  </g>"
            "  <rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"rgba(0,0,255,0.5)\"/>"
            "</svg>";
        rc = img_decode(s_grp, sizeof(s_grp) - 1, &im);
        check(rc == IMG_OK && im.w == 40 && im.h == 40, "svg_grp_decode", "rc=%d", rc);
        if (rc == IMG_OK) {
            /* Rect in viewBox: x=[5..13], y=[2..10] -> device x=[10..26), y=[4..20). */
            /* Pixel (12, 10): green over transparent, then rgba(0,0,255,0.5) over green -> ~0x008080, alpha 255. */
            uint32_t c = im.px[10 * 40 + 12];
            int g = (int)((c >> 8) & 0xffu);
            int b = (int)(c & 0xffu);
            check(im.alpha[10 * 40 + 12] == 255 && g >= 120 && g <= 135 && b >= 120 && b <= 135,
                  "svg_grp_blend", "px=%06x a=%u", c, im.alpha[10 * 40 + 12]);
            /* Pixel (2, 2): only rgba(0,0,255,0.5) over transparent -> pure blue, alpha ~128. */
            check(im.px[2 * 40 + 2] == 0x0000ffu &&
                  im.alpha[2 * 40 + 2] >= 127 && im.alpha[2 * 40 + 2] <= 128,
                  "svg_rgba_over_empty", "px=%06x a=%u",
                  im.px[2 * 40 + 2], im.alpha[2 * 40 + 2]);
            img_free(&im);
        }
    }

    /* 3e. rotate, matrix, evenodd fill-rule, S/s, T/t, group opacity, style="". */
    {
        static const char s_adv[] =
            "<svg width=\"48\" height=\"48\">"
            "  <g opacity=\"0.5\" transform=\"rotate(90, 24, 24)\">"
            "    <rect x=\"24\" y=\"14\" width=\"10\" height=\"10\" style=\"fill:#ff0000\"/>"
            "  </g>"
            "  <path d=\"M2 2 H20 V20 H2 Z M6 6 H16 V16 H6 Z\" fill-rule=\"evenodd\" fill=\"#00ff00\"/>"
            "  <path d=\"M26 40 C26 30 34 30 34 40 S42 50 42 40\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"2\"/>"
            "  <path d=\"M2 40 Q6 30 10 40 T18 40\" fill=\"none\" stroke=\"#ffff00\" stroke-width=\"2\"/>"
            "</svg>";
        struct img_info pinfo;
        rc = svg_probe(s_adv, sizeof(s_adv) - 1, &pinfo);
        check(rc == IMG_OK && pinfo.fmt == IMG_FMT_SVG && pinfo.w == 48 && pinfo.h == 48,
              "svg_direct_probe", "rc=%d", rc);
        rc = svg_decode(s_adv, sizeof(s_adv) - 1, &im);
        check(rc == IMG_OK, "svg_adv_decode", "rc=%d", rc);
        if (rc == IMG_OK) {
            /* rect [24..34, 14..24] rotated 90 deg around (24,24) => [24..34, 24..34]. */
            check(im.px[29 * 48 + 29] == 0xff0000u &&
                  im.alpha[29 * 48 + 29] >= 127 && im.alpha[29 * 48 + 29] <= 128,
                  "svg_rotate_opacity", "px=%06x a=%u",
                  im.px[29 * 48 + 29], im.alpha[29 * 48 + 29]);
            /* evenodd hole: (4,4) green, center hole (11,11) transparent. */
            check(im.px[4 * 48 + 4] == 0x00ff00u && im.alpha[4 * 48 + 4] == 255 &&
                  im.alpha[11 * 48 + 11] == 0,
                  "svg_evenodd_hole", "px=%06x a_hole=%u",
                  im.px[4 * 48 + 4], im.alpha[11 * 48 + 11]);
            /* T smooth quad: peak at (14, 45) or midpoint (10, 40) stroked yellow. */
            check(im.px[40 * 48 + 10] == 0xffff00u,
                  "svg_path_t_stroke", "px=%06x", im.px[40 * 48 + 10]);
            img_free(&im);
        }
    }
}

/* ---------------------------------------------------------- 4. QOI tests */

static void test_qoi(void)
{
    /* Exercise RUN (initial), RGB, DIFF, LUMA, INDEX, RGBA, wraparound. */
    static const uint32_t img4x4[16] = {
        0xff000000u, 0xff000000u, 0xff000000u, /* initial run of 3 black */
        0xff804020u,                           /* RGB */
        0xff813f1fu,                           /* DIFF (+1, -1, -1) */
        0xff914d2eu,                           /* LUMA (dg=+14, dr=+16, db=+15) */
        0xff804020u,                           /* INDEX repeat */
        0x80ff01ffu,                           /* RGBA (alpha 128) */
        0x8000ff00u,                           /* RGBA wrap (r:255->0, g:1->255) */
        0x8000ff00u, 0x8000ff00u,              /* RUN of RGBA */
        0xff000000u,                           /* INDEX/RGBA */
        0x00123456u,                           /* alpha=0 */
        0xff813f1fu,                           /* INDEX */
        0xff804121u,                           /* DIFF from 0x813f1f */
        0xffffffffu                            /* white */
    };
    unsigned long qlen = 0, i;
    uint8_t *qoi = make_qoi(4, 4, 4, 0, img4x4, &qlen);
    struct img_info info;
    struct image im;
    int rc;

    check(qoi != 0, "qoi_encode", "oom");
    if (qoi) {
        rc = img_probe(qoi, qlen, &info);
        check(rc == IMG_OK && info.fmt == IMG_FMT_QOI &&
              info.w == 4 && info.h == 4 && info.has_alpha == 1,
              "qoi_probe_rgba", "rc=%d %dx%d a=%d", rc, info.w, info.h, info.has_alpha);

        rc = img_decode(qoi, qlen, &im);
        check(rc == IMG_OK && im.w == 4 && im.h == 4 && im.has_alpha == 1,
              "qoi_decode_rgba", "rc=%d", rc);
        if (rc == IMG_OK) {
            int match = 1;
            for (i = 0; i < 16; i++) {
                uint32_t exp_rgb = img4x4[i] & 0xffffffu;
                uint8_t exp_a = (uint8_t)(img4x4[i] >> 24);
                if (im.px[i] != exp_rgb || im.alpha[i] != exp_a) {
                    match = 0;
                    break;
                }
            }
            check(match, "qoi_rgba_pixels", "mismatch at %lu", i);
            img_free(&im);
        }
        free(qoi);
    }

    /* RGB-only QOI (channels = 3, linear colorspace = 1). */
    qoi = make_qoi(4, 4, 3, 1, img4x4, &qlen);
    if (qoi) {
        rc = img_probe(qoi, qlen, &info);
        check(rc == IMG_OK && info.has_alpha == 0, "qoi_probe_rgb", "rc=%d a=%d", rc, info.has_alpha);
        rc = img_decode(qoi, qlen, &im);
        check(rc == IMG_OK && !im.has_alpha && im.alpha == 0 &&
              im.px[3] == 0x804020u && im.px[15] == 0xffffffu,
              "qoi_decode_rgb", "rc=%d", rc);
        img_free(&im);
        free(qoi);
    }
}

/* ---------------------------------------------------------- 5. Netpbm tests */

static void test_pnm(void)
{
    struct img_info info;
    struct image im;
    int rc;

    /* P1 ASCII PBM (0=white, 1=black, packed bits, comments). */
    {
        static const char p1[] =
            "P1\n# pbm test\n3 2\n"
            "010\n"
            "1 0 1\n";
        rc = img_probe(p1, sizeof(p1) - 1, &info);
        check(rc == IMG_OK && info.fmt == IMG_FMT_PNM && info.w == 3 && info.h == 2 && !info.has_alpha,
              "p1_probe", "rc=%d", rc);
        rc = img_decode(p1, sizeof(p1) - 1, &im);
        check(rc == IMG_OK && im.px[0] == 0xffffffu && im.px[1] == 0x000000u &&
              im.px[2] == 0xffffffu && im.px[3] == 0x000000u &&
              im.px[4] == 0xffffffu && im.px[5] == 0x000000u,
              "p1_decode", "rc=%d", rc);
        img_free(&im);
    }

    /* P2 ASCII PGM (maxval = 15 scaling + comments). */
    {
        static const char p2[] =
            "P2 # gray\n2 2\n# max\n15\n0 15\n5 # mid\n10\n";
        rc = img_decode(p2, sizeof(p2) - 1, &im);
        check(rc == IMG_OK && im.px[0] == 0x000000u && im.px[1] == 0xffffffu &&
              im.px[2] == 0x555555u && im.px[3] == 0xaaaaaau,
              "p2_decode", "rc=%d px2=%06x", rc, im.px ? im.px[2] : 0);
        img_free(&im);
    }

    /* P3 ASCII PPM. */
    {
        static const char p3[] = "P3 2 1 255 255 0 0 0 128 255";
        rc = img_decode(p3, sizeof(p3) - 1, &im);
        check(rc == IMG_OK && im.px[0] == 0xff0000u && im.px[1] == 0x0080ffu,
              "p3_decode", "rc=%d", rc);
        img_free(&im);
    }

    /* P4 binary PBM (5x2 non-byte-aligned width). */
    {
        static const uint8_t p4[] = {
            'P', '4', '\n', '5', ' ', '2', '\n',
            0xa8, /* 10101xxx => B W B W B */
            0x50  /* 01010xxx => W B W B W */
        };
        rc = img_decode(p4, sizeof(p4), &im);
        check(rc == IMG_OK && im.w == 5 && im.h == 2 &&
              im.px[0] == 0x000000u && im.px[1] == 0xffffffu && im.px[4] == 0x000000u &&
              im.px[5] == 0xffffffu && im.px[6] == 0x000000u && im.px[9] == 0xffffffu,
              "p4_decode", "rc=%d", rc);
        img_free(&im);
    }

    /* P5 binary PGM (8-bit and 16-bit BE). */
    {
        static const uint8_t p5_8[] = {
            'P', '5', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n',
            0x20, 0x0a /* starts with ' ' and '\n' to verify single ws skip! */
        };
        static const uint8_t p5_16[] = {
            'P', '5', '\n', '2', ' ', '1', '\n', '6', '5', '5', '3', '5', '\n',
            0xff, 0xff, 0x80, 0x00
        };
        rc = img_decode(p5_8, sizeof(p5_8), &im);
        check(rc == IMG_OK && im.px[0] == 0x202020u && im.px[1] == 0x0a0a0au,
              "p5_8_decode", "rc=%d px0=%06x", rc, im.px ? im.px[0] : 0);
        img_free(&im);

        rc = img_decode(p5_16, sizeof(p5_16), &im);
        check(rc == IMG_OK && im.px[0] == 0xffffffu && im.px[1] == 0x808080u,
              "p5_16_decode", "rc=%d px1=%06x", rc, im.px ? im.px[1] : 0);
        img_free(&im);
    }

    /* P6 binary PPM (8-bit starting with 0x20/0x0a + 16-bit). */
    {
        static const uint8_t p6_8[] = {
            'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', ' ',
            0x20, 0x0a, 0xff, 0x00, 0x80, 0x40
        };
        static const uint8_t p6_16[] = {
            'P', '6', '\n', '1', ' ', '1', '\n', '1', '0', '0', '0', '\n',
            0x03, 0xe8, 0x01, 0xf4, 0x00, 0x00
        };
        rc = img_decode(p6_8, sizeof(p6_8), &im);
        check(rc == IMG_OK && im.px[0] == 0x200affu && im.px[1] == 0x008040u,
              "p6_8_decode", "rc=%d px0=%06x", rc, im.px ? im.px[0] : 0);
        img_free(&im);

        rc = img_decode(p6_16, sizeof(p6_16), &im);
        check(rc == IMG_OK && im.px[0] == 0xff8000u,
              "p6_16_decode", "rc=%d px0=%06x", rc, im.px ? im.px[0] : 0);
        img_free(&im);
    }
}

/* ---------------------------------------------------------- 6. negative & fuzz */

static void test_negative_and_truncated(void)
{
    struct image im;
    struct img_info info;
    static const uint32_t px1[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
    unsigned long qlen = 0, i;
    uint8_t *qoi = make_qoi(2, 2, 4, 0, px1, &qlen);

    /* Null & invalid args. */
    check(img_decode(0, 10, &im) == IMG_ERR_INVAL, "dec_null_data", "bad");
    check(img_decode("abc", 3, 0) == IMG_ERR_INVAL, "dec_null_out", "bad");
    check(img_probe(0, 10, &info) == IMG_ERR_INVAL, "probe_null_data", "bad");
    check(img_probe("abc", 3, 0) == IMG_ERR_INVAL, "probe_null_out", "bad");
    check(img_decode("abc", IMG_MAX_INPUT + 1UL, &im) == IMG_ERR_TOOBIG, "dec_toobig_input", "bad");

    /* Truncated QOI across every prefix length < qlen. */
    if (qoi) {
        for (i = 0; i < qlen; i++) {
            int rc = qoi_decode(qoi, i, &im);
            check(rc != IMG_OK && im.px == 0 && im.alpha == 0,
                  "qoi_trunc", "len=%lu rc=%d", i, rc);
        }
        /* Corrupt end marker & bad channels/colorspace & huge dims. */
        qoi[qlen - 1] = 0x02;
        check(qoi_decode(qoi, qlen, &im) == IMG_ERR_CORRUPT && im.px == 0,
              "qoi_bad_end", "bad");
        qoi[qlen - 1] = 0x01;
        qoi[12] = 5;
        check(qoi_decode(qoi, qlen, &im) == IMG_ERR_CORRUPT, "qoi_bad_ch", "bad");
        qoi[12] = 4;
        qoi[13] = 2;
        check(qoi_decode(qoi, qlen, &im) == IMG_ERR_CORRUPT, "qoi_bad_cs", "bad");
        qoi[13] = 0;
        wr32be(qoi + 4, 9000);
        check(qoi_decode(qoi, qlen, &im) == IMG_ERR_TOOBIG, "qoi_toobig_dim", "bad");
        free(qoi);
    }

    /* Corrupt / truncated ICO. */
    {
        static const uint8_t ico_zero_cnt[6] = { 0, 0, 1, 0, 0, 0 };
        static const uint8_t ico_oob[22] = {
            0, 0, 1, 0, 1, 0,
            16, 16, 0, 0, 1, 0, 32, 0, 100, 0, 0, 0, 22, 0, 0, 0
        };
        static const uint8_t ico_bad_bpp[62] = {
            0, 0, 1, 0, 1, 0,
            1, 1, 0, 0, 1, 0, 16, 0, 40, 0, 0, 0, 22, 0, 0, 0,
            40, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 16, 0, 0, 0, 0, 0
        };
        check(img_decode(ico_zero_cnt, sizeof(ico_zero_cnt), &im) == IMG_ERR_CORRUPT,
              "ico_zero_cnt", "bad");
        check(img_decode(ico_oob, sizeof(ico_oob), &im) == IMG_ERR_TRUNCATED,
              "ico_oob", "bad");
        check(img_decode(ico_bad_bpp, sizeof(ico_bad_bpp), &im) == IMG_ERR_UNSUPPORTED,
              "ico_16bpp_unsup", "bad");
        for (i = 1; i < 10; i++) {
            check(ico_decode(ico_oob, i, &im) != IMG_OK && im.px == 0,
                  "ico_trunc_prefix", "i=%lu", i);
        }
    }

    /* Corrupt / truncated SVG. */
    {
        static const char svg_unclosed[] = "<svg width=\"16\" height=\"16\"><rect";
        static const char svg_no_close[] = "<svg width=\"16\" height=\"16\"><rect width=\"5\" height=\"5\"/>";
        static const char svg_not_svg[] = "<html><body/></html>";
        static const char svg_bad_path[] = "<svg width=\"16\" height=\"16\"><path d=\"L 1 2 Z Q\"/></svg>";

        check(svg_decode(svg_unclosed, sizeof(svg_unclosed) - 1, &im) == IMG_ERR_TRUNCATED && im.px == 0,
              "svg_unclosed_tag", "bad");
        check(svg_decode(svg_no_close, sizeof(svg_no_close) - 1, &im) == IMG_ERR_TRUNCATED && im.px == 0,
              "svg_missing_end", "bad");
        check(svg_decode(svg_not_svg, sizeof(svg_not_svg) - 1, &im) == IMG_ERR_FORMAT,
              "svg_not_svg", "bad");
        check(svg_decode(svg_bad_path, sizeof(svg_bad_path) - 1, &im) == IMG_OK,
              "svg_graceful_bad_d", "bad");
        img_free(&im);
    }

    /* Corrupt / oversized / truncated PNM. */
    {
        static const char pnm_huge[] = "P6\n8000 8000\n255\n\x00\x00\x00";
        static const char pnm_toobig[] = "P6\n9000 1\n255\n";
        static const char pnm_badmax[] = "P5\n2 2\n70000\n1234";
        static const char pnm_zeromax[] = "P2\n1 1\n0\n0";
        static const char pnm_trunc_p3[] = "P3\n2 2\n255\n1 2 3";
        static const char pnm_bad_p1[] = "P1\n2 1\n0 2";

        check(img_decode(pnm_huge, sizeof(pnm_huge) - 1, &im) == IMG_ERR_TRUNCATED && im.px == 0,
              "pnm_huge_trunc_no_oom", "bad");
        check(img_decode(pnm_toobig, sizeof(pnm_toobig) - 1, &im) == IMG_ERR_TOOBIG,
              "pnm_toobig_dim", "bad");
        check(img_decode(pnm_badmax, sizeof(pnm_badmax) - 1, &im) == IMG_ERR_CORRUPT,
              "pnm_bad_maxval", "bad");
        check(img_decode(pnm_zeromax, sizeof(pnm_zeromax) - 1, &im) == IMG_ERR_CORRUPT,
              "pnm_zero_maxval", "bad");
        check(img_decode(pnm_trunc_p3, sizeof(pnm_trunc_p3) - 1, &im) == IMG_ERR_TRUNCATED && im.px == 0,
              "pnm_trunc_p3", "bad");
        check(img_decode(pnm_bad_p1, sizeof(pnm_bad_p1) - 1, &im) == IMG_ERR_CORRUPT && im.px == 0,
              "pnm_bad_p1_digit", "bad");
    }

    /* Deterministic byte-mutation fuzz across ICO, SVG, QOI, PNM. */
    {
        static const char seed_svg[] =
            "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">"
            "<g fill=\"red\"><rect x=\"1\" y=\"1\" width=\"10\" height=\"10\" rx=\"2\"/>"
            "<path d=\"M2 2 Q12 20 22 2 C22 12 2 12 2 22 Z\"/></g></svg>";
        static const uint8_t seed_p6[] = {
            'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
        };
        static const uint8_t seed_ico[] = {
            0, 0, 1, 0, 1, 0, 2, 2, 0, 0, 1, 0, 32, 0, 64, 0, 0, 0, 22, 0, 0, 0,
            40, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 1, 0, 32, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            10, 20, 30, 255, 40, 50, 60, 128, 70, 80, 90, 0, 1, 2, 3, 255,
            0x80, 0, 0, 0, 0, 0, 0, 0
        };
        const void *seeds[4];
        unsigned long slens[4];
        uint32_t rng = 0x13579bdfu;
        int s, round;

        seeds[0] = seed_svg; slens[0] = sizeof(seed_svg) - 1;
        seeds[1] = seed_p6;  slens[1] = sizeof(seed_p6);
        seeds[2] = seed_ico; slens[2] = sizeof(seed_ico);
        seeds[3] = 0;        slens[3] = 0;

        /* Ensure qoi was kept or regenerate simple qoi if freed above. */
        {
            static const uint32_t fpx[4] = { 0xff102030u, 0x80405060u, 0xff708090u, 0xffffffffu };
            unsigned long fqlen = 0;
            uint8_t *fqoi = make_qoi(2, 2, 4, 0, fpx, &fqlen);
            seeds[3] = fqoi;
            slens[3] = fqlen;

            for (s = 0; s < 4; s++) {
                uint8_t *mut = (uint8_t *)malloc(slens[s] + 1);
                if (!mut || !seeds[s]) { free(mut); continue; }
                for (round = 0; round < 100; round++) {
                    unsigned long k, mlen;
                    memcpy(mut, seeds[s], slens[s]);
                    mut[slens[s]] = 0;
                    for (k = 0; k < 3; k++) {
                        rng = rng * 1664525u + 1013904223u;
                        mut[rng % slens[s]] ^= (uint8_t)(rng >> 24);
                    }
                    rng = rng * 1664525u + 1013904223u;
                    mlen = (round % 4 == 0) ? (rng % (slens[s] + 1)) : slens[s];
                    img_probe(mut, mlen, &info);
                    if (img_decode(mut, mlen, &im) == IMG_OK) {
                        check(im.w > 0 && im.h > 0 && im.px != 0,
                              "fuzz_valid_out", "s=%d r=%d", s, round);
                        img_free(&im);
                    } else {
                        check(im.px == 0 && im.alpha == 0,
                              "fuzz_err_clean", "s=%d r=%d", s, round);
                    }
                }
                free(mut);
            }
            free(fqoi);
        }
        check(1, "fuzz_complete", "400 mutations safe");
    }
}

int main(void)
{
    printf("=== libimg multi-format test suite ===\n");
    test_sniff_and_names();
    test_ico();
    test_svg();
    test_qoi();
    test_pnm();
    test_negative_and_truncated();
    printf("Result: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
